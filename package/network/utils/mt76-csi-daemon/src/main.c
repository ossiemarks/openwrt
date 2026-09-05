#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <net/if.h>

#include "config.h"
#include "state.h"
#include "queue.h"
#include "netlink.h"
#include "udp.h"
#include "mqtt.h"
#include "ubus.h"
#include "rest.h"
#include "analysis/presence.h"
#include "analysis/position.h"
#include "analysis/gesture.h"
#include "analysis/vitals.h"

csi_state_t  g_state;
csi_config_t g_config;

static volatile bool running = true;

static void on_signal(int sig)
{
    (void)sig;
    running = false;
}

struct reader_args {
    csi_queue_t *q;
};

static void *reader_thread(void *arg)
{
    struct reader_args *ra = arg;
    csi_nl_reader_run(g_config.iface, ra->q, &running);
    return NULL;
}

/* Wait for a usable AP interface: either the configured one, or (interface
 * = auto / configured one missing) the AP with the most stations. Wifi can
 * come up after us at boot, so poll for a while before giving up. */
static int resolve_iface(void)
{
    char found[CSI_IFACE_LEN];
    int tries;

    for (tries = 0; tries < 60 && running; tries++) {
        if (!g_config.iface_auto && if_nametoindex(g_config.iface))
            return 0;

        if (csi_nl_find_ap_iface(found, sizeof(found)) >= 0) {
            if (strcmp(found, g_config.iface))
                fprintf(stderr, "csi: using AP interface %s%s\n", found,
                        g_config.iface_auto ? "" : " (configured one not found)");
            snprintf(g_config.iface, sizeof(g_config.iface), "%s", found);
            return 0;
        }
        sleep(1);
    }
    return -1;
}

static void csi_start_on_iface(void)
{
    /* Start from a clean firmware state, then the frame-type filter: the
     * firmware silently reports nothing until cfg item 3 has been set. */
    csi_nl_disable(g_config.iface);
    csi_nl_set_frame_type(g_config.iface, g_config.frame_type_v1,
                          g_config.frame_type_v2);
}

/* Stations currently installed in the firmware MAC filter. */
static uint8_t active_sta[CSI_MAX_STATIONS][6];
static int     active_count;

static int sta_index(const uint8_t *mac)
{
    for (int i = 0; i < active_count; i++)
        if (!memcmp(active_sta[i], mac, 6))
            return i;
    return -1;
}

static void sta_add(const uint8_t *mac)
{
    if (active_count >= CSI_MAX_STATIONS)
        return;
    if (csi_nl_sta_filter(g_config.iface, mac, true, g_config.sta_interval))
        return;
    memcpy(active_sta[active_count++], mac, 6);
    fprintf(stderr, "csi: tracking %02x:%02x:%02x:%02x:%02x:%02x on %s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_config.iface);
}

static void sta_remove(int idx)
{
    const uint8_t *mac = active_sta[idx];

    fprintf(stderr, "csi: dropping %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    csi_nl_sta_filter(g_config.iface, mac, false, 0);
    active_count--;
    if (idx != active_count)
        memcpy(active_sta[idx], active_sta[active_count], 6);
}

/* Bring the firmware MAC filter in line with the wanted station set:
 * the fixed list from config, or every associated station when auto. */
static void sync_stations(void)
{
    uint8_t want[CSI_MAX_STATIONS][6];
    int n, i;

    if (g_config.stations_auto) {
        n = csi_nl_list_stations(g_config.iface, want, CSI_MAX_STATIONS);
        if (n < 0)
            return;
        if (n > CSI_MAX_STATIONS)
            n = CSI_MAX_STATIONS;
    } else {
        n = g_config.station_count;
        memcpy(want, g_config.stations, n * 6);
    }

    for (i = active_count - 1; i >= 0; i--) {
        int j;
        for (j = 0; j < n; j++)
            if (!memcmp(active_sta[i], want[j], 6))
                break;
        if (j == n)
            sta_remove(i);
    }

    for (i = 0; i < n; i++)
        if (sta_index(want[i]) < 0)
            sta_add(want[i]);
}

/* With interface = auto and nothing to track yet, keep looking for an AP
 * interface that has stations (clients may associate to the other radio
 * after we started). The reader re-resolves the name on every pass. */
static void maybe_switch_iface(void)
{
    char found[CSI_IFACE_LEN];
    int stas;

    if (!g_config.iface_auto || active_count)
        return;

    stas = csi_nl_find_ap_iface(found, sizeof(found));
    if (stas <= 0 || !strcmp(found, g_config.iface))
        return;

    fprintf(stderr, "csi: switching to %s (%d station%s)\n",
            found, stas, stas == 1 ? "" : "s");
    csi_nl_disable(g_config.iface);
    snprintf(g_config.iface, sizeof(g_config.iface), "%s", found);
    csi_start_on_iface();
    sync_stations();
    csi_nl_enable(g_config.iface);
}

static void *dispatch_thread(void *arg)
{
    csi_queue_t *q = arg;
    csi_frame_t frame;
    struct timespec t0;
    unsigned long last_count = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (running) {
        if (queue_pop(q, &frame, 200)) {
            pthread_mutex_lock(&g_state.lock);
            g_state.frames_total++;
            pthread_mutex_unlock(&g_state.lock);

            udp_send(&frame);
            mqtt_publish_raw(&frame);

            presence_process(&frame);
            position_process(&frame);
            gesture_process(&frame);
            vitals_process(&frame);
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - t0.tv_sec) +
                    (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (dt >= 1.0) {
            pthread_mutex_lock(&g_state.lock);
            g_state.frame_rate = (g_state.frames_total - last_count) / dt;
            last_count = g_state.frames_total;
            pthread_mutex_unlock(&g_state.lock);
            t0 = now;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *conf_path = "/etc/mt76-csi.conf";
    const char *cli_macs[CSI_MAX_STATIONS];
    int cli_mac_count = 0;
    int cli_interval = -1;
    int opt;

    while ((opt = getopt(argc, argv, "c:m:i:h")) != -1) {
        switch (opt) {
        case 'c': conf_path = optarg; break;
        case 'm':
            if (cli_mac_count < CSI_MAX_STATIONS)
                cli_macs[cli_mac_count++] = optarg;
            break;
        case 'i': cli_interval = atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "usage: %s [-c conf] [-m sta_mac]... [-i interval_us]\n"
                "  -m overrides the [csi] stations list; -i overrides sta_interval\n",
                argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (config_load(conf_path, &g_config) < 0)
        fprintf(stderr, "csi: config %s not found, using defaults\n", conf_path);

    if (cli_mac_count) {
        g_config.station_count = 0;
        for (int i = 0; i < cli_mac_count; i++)
            if (config_add_station(&g_config, cli_macs[i]))
                fprintf(stderr, "csi: bad station '%s'\n", cli_macs[i]);
    }
    if (cli_interval >= 0)
        g_config.sta_interval = (unsigned)cli_interval;

    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);
    g_state.active_zone = -1;
    g_state.vitals_zone = g_config.zone_count > 0 ? 0 : -1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    presence_init();
    position_init();
    gesture_init();
    vitals_init();

    if (udp_init(&g_config) < 0)
        fprintf(stderr, "csi: UDP init failed\n");
    if (mqtt_start(&g_config) < 0)
        fprintf(stderr, "csi: MQTT start failed\n");
    ubus_start(&g_config, &running);
    rest_start(&g_config, &running);

    csi_queue_t *q = queue_create();
    if (!q) {
        fprintf(stderr, "csi: queue alloc failed\n");
        return 1;
    }

    if (resolve_iface() < 0) {
        fprintf(stderr, "csi: no AP interface found (configured: %s)\n",
                g_config.iface);
        return 1;
    }

    csi_start_on_iface();
    sync_stations();
    if (!active_count)
        fprintf(stderr, "csi: no stations in MAC filter yet on %s; "
                "firmware will not report CSI until one associates\n",
                g_config.iface);

    if (csi_nl_enable(g_config.iface) < 0)
        fprintf(stderr, "csi: could not enable CSI on %s\n", g_config.iface);

    pthread_t reader_tid, dispatch_tid;
    struct reader_args ra = { .q = q };
    pthread_create(&reader_tid, NULL, reader_thread, &ra);
    pthread_create(&dispatch_tid, NULL, dispatch_thread, q);

    for (unsigned tick = 0; running; tick++) {
        sleep(1);
        /* associated set changes rarely; re-sync every 5 s */
        if (tick % 5 == 4) {
            maybe_switch_iface();
            if (g_config.stations_auto)
                sync_stations();
        }
    }

    queue_shutdown(q);
    pthread_join(reader_tid, NULL);
    pthread_join(dispatch_tid, NULL);

    while (active_count)
        sta_remove(active_count - 1);
    csi_nl_disable(g_config.iface);

    rest_stop();
    ubus_stop();
    mqtt_stop();
    udp_deinit();

    presence_deinit();
    position_deinit();
    gesture_deinit();
    vitals_deinit();

    queue_destroy(q);
    return 0;
}
