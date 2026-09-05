#include <string.h>
#include <stdio.h>
#include "stubs.h"
#include "../src/mqtt.h"

csi_state_t  g_state;
csi_config_t g_config;

#define MAX_TOPICS 8
#define TOPIC_LEN  24
#define EVENT_LEN  256

static struct {
    char topic[TOPIC_LEN];
    char last[EVENT_LEN];
    int  count;
} events[MAX_TOPICS];

static int event_topics;
static int state_inited;

void stub_reset(void)
{
    if (!state_inited) {
        pthread_mutex_init(&g_state.lock, NULL);
        state_inited = 1;
    }
    pthread_mutex_t saved = g_state.lock;
    memset(&g_state, 0, sizeof(g_state));
    g_state.lock = saved;
    g_state.active_zone = -1;
    g_state.vitals_zone = -1;

    memset(&g_config, 0, sizeof(g_config));
    g_config.zone_count = 4;
    g_config.bandwidth = 20;

    stub_clear_events();
}

void stub_clear_events(void)
{
    memset(events, 0, sizeof(events));
    event_topics = 0;
}

static int topic_slot(const char *subtopic, int create)
{
    for (int i = 0; i < event_topics; i++) {
        if (strcmp(events[i].topic, subtopic) == 0)
            return i;
    }
    if (!create || event_topics >= MAX_TOPICS)
        return -1;
    snprintf(events[event_topics].topic, TOPIC_LEN, "%s", subtopic);
    return event_topics++;
}

const char *stub_last_event(const char *subtopic)
{
    int i = topic_slot(subtopic, 0);
    return i < 0 ? NULL : events[i].last;
}

int stub_event_count(const char *subtopic)
{
    int i = topic_slot(subtopic, 0);
    return i < 0 ? 0 : events[i].count;
}

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
    int i = topic_slot(subtopic, 1);
    if (i < 0)
        return;
    snprintf(events[i].last, EVENT_LEN, "%s", json);
    events[i].count++;
}

void mqtt_stop(void) {}
