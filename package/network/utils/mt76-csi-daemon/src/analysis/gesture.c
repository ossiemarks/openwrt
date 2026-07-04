#include <math.h>
#include <string.h>
#include <stdio.h>
#include "gesture.h"
#include "../state.h"
#include "../mqtt.h"

/* Temporal pattern classifier over a short amplitude-energy window.
 * Distinguishes coarse gestures by the shape of the motion-energy burst:
 * a single symmetric peak -> "wave", a rising ramp -> "push",
 * a falling ramp -> "pull". Intentionally lightweight (2-antenna HW). */

#define GWIN 48
#define ENERGY_ON  0.12f
#define ENERGY_OFF 0.05f

static float ewin[GWIN];
static int   wpos, wfull;
static bool  in_gesture;
static float burst[GWIN];
static int   burst_len;

static float energy(const csi_frame_t *f)
{
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > CSI_MAX_SUBCARRIERS)
        n = CSI_MAX_SUBCARRIERS;
    float a = 0.0f;
    for (int i = 0; i < n; i++)
        a += sqrtf((float)f->data_i[i] * f->data_i[i] +
                   (float)f->data_q[i] * f->data_q[i]);
    return a / n;
}

void gesture_init(void)
{
    memset(ewin, 0, sizeof(ewin));
    wpos = wfull = 0;
    in_gesture = false;
    burst_len = 0;
}

static void classify(void)
{
    if (burst_len < 4)
        return;

    /* peak position within burst */
    int peak = 0;
    float pv = burst[0];
    for (int i = 1; i < burst_len; i++) {
        if (burst[i] > pv) {
            pv = burst[i];
            peak = i;
        }
    }
    float first = burst[0], last = burst[burst_len - 1];

    const char *label;
    float conf;
    float rel = (float)peak / (burst_len - 1);
    if (rel > 0.35f && rel < 0.65f) {
        label = "wave";
        conf = 0.80f;
    } else if (last > first) {
        label = "push";
        conf = 0.70f;
    } else {
        label = "pull";
        conf = 0.70f;
    }

    pthread_mutex_lock(&g_state.lock);
    snprintf(g_state.last_gesture, sizeof(g_state.last_gesture), "%s", label);
    g_state.gesture_confidence = conf;
    pthread_mutex_unlock(&g_state.lock);

    char json[128];
    snprintf(json, sizeof(json),
        "{\"gesture\":\"%s\",\"confidence\":%.2f}", label, conf);
    mqtt_publish_event("gesture", json);
}

void gesture_process(const csi_frame_t *frame)
{
    float e = energy(frame);

    ewin[wpos] = e;
    wpos = (wpos + 1) % GWIN;
    if (!wfull && wpos == 0)
        wfull = 1;

    int n = wfull ? GWIN : wpos;
    if (n < 8)
        return;

    float mean = 0.0f;
    for (int i = 0; i < n; i++)
        mean += ewin[i];
    mean /= n;
    float norm = fabsf(e - mean) / (mean + 1e-6f);

    if (!in_gesture && norm > ENERGY_ON) {
        in_gesture = true;
        burst_len = 0;
    }
    if (in_gesture) {
        if (burst_len < GWIN)
            burst[burst_len++] = norm;
        if (norm < ENERGY_OFF) {
            classify();
            in_gesture = false;
        }
    }
}

void gesture_deinit(void) {}
