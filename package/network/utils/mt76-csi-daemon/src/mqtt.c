#include <stdio.h>
#include <string.h>
#include "mqtt.h"

/*
 * MQTT support is compiled in only when WITH_MQTT is defined (the OpenWrt
 * package Makefile sets it and depends on libmosquitto). When built without
 * it, these become no-ops so the daemon still runs with UDP/ubus/REST — MQTT
 * is disabled by default in the config anyway.
 */
#ifdef WITH_MQTT
#include <mosquitto.h>

static struct mosquitto *mosq;
static char topic_raw[64];
static char topic_event[64];
static bool enabled;

int mqtt_start(const csi_config_t *cfg)
{
    enabled = cfg->mqtt_enabled;
    if (!enabled)
        return 0;

    snprintf(topic_raw, sizeof(topic_raw), "%s", cfg->mqtt_topic_raw);
    snprintf(topic_event, sizeof(topic_event), "%s", cfg->mqtt_topic_event);

    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq)
        return -1;

    if (mosquitto_connect(mosq, cfg->mqtt_broker, cfg->mqtt_port, 60)) {
        mosquitto_destroy(mosq);
        mosq = NULL;
        return -1;
    }
    mosquitto_loop_start(mosq);
    return 0;
}

void mqtt_publish_raw(const csi_frame_t *f)
{
    if (!enabled || !mosq)
        return;
    /* compact: ts,rssi,snr,data_num — full raw goes over UDP */
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%u,\"rssi\":%d,\"snr\":%u,\"n\":%u}",
        f->ts, f->rssi, f->snr, f->data_num);
    mosquitto_publish(mosq, NULL, topic_raw, n, buf, 0, false);
}

void mqtt_publish_event(const char *subtopic, const char *json)
{
    if (!enabled || !mosq)
        return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", topic_event, subtopic);
    mosquitto_publish(mosq, NULL, topic, strlen(json), json, 0, false);
}

void mqtt_stop(void)
{
    if (mosq) {
        mosquitto_loop_stop(mosq, true);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosq = NULL;
        mosquitto_lib_cleanup();
    }
}

#else /* !WITH_MQTT */

int mqtt_start(const csi_config_t *cfg)
{
    (void)cfg;
    return 0;
}

void mqtt_publish_raw(const csi_frame_t *f)
{
    (void)f;
}

void mqtt_publish_event(const char *subtopic, const char *json)
{
    (void)subtopic;
    (void)json;
}

void mqtt_stop(void)
{
}

#endif /* WITH_MQTT */
