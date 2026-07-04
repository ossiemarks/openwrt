#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>

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
    const char *sta_mac = NULL;
    unsigned interval = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:m:i:h")) != -1) {
        switch (opt) {
        case 'c': conf_path = optarg; break;
        case 'm': sta_mac = optarg; break;
        case 'i': interval = (unsigned)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "usage: %s [-c conf] [-m sta_mac] [-i interval_us]\n",
                argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (config_load(conf_path, &g_config) < 0)
        fprintf(stderr, "csi: config %s not found, using defaults\n", conf_path);

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

    if (csi_nl_enable(g_config.iface, sta_mac, interval) < 0)
        fprintf(stderr, "csi: could not enable CSI on %s\n", g_config.iface);

    pthread_t reader_tid, dispatch_tid;
    struct reader_args ra = { .q = q };
    pthread_create(&reader_tid, NULL, reader_thread, &ra);
    pthread_create(&dispatch_tid, NULL, dispatch_thread, q);

    while (running)
        sleep(1);

    queue_shutdown(q);
    pthread_join(reader_tid, NULL);
    pthread_join(dispatch_tid, NULL);

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
