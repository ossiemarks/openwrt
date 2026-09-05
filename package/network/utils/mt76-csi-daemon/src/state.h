#ifndef CSI_STATE_H
#define CSI_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include "config.h"

typedef struct {
    pthread_mutex_t lock;

    /* presence */
    bool  present;
    float presence_confidence;
    int   person_count;

    /* position */
    int   active_zone;              /* -1 = none */
    float zone_confidence;
    bool  zone_occupied[CSI_MAX_ZONES];

    /* gesture */
    char  last_gesture[24];
    float gesture_confidence;
    uint32_t gesture_ts;

    /* vitals */
    int   vitals_zone;              /* focused zone, -1 = none */
    bool  vitals_feasible;
    char  vitals_reason[32];
    int   respiration_bpm;
    int   heart_rate_bpm;
    float vitals_confidence;

    /* health */
    unsigned long frames_total;
    double frame_rate;
} csi_state_t;

extern csi_state_t g_state;
extern csi_config_t g_config;

#endif /* CSI_STATE_H */
