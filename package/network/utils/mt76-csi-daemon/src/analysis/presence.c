#include <math.h>
#include <string.h>
#include <stdio.h>
#include "presence.h"
#include "../state.h"
#include "../mqtt.h"

#define WINDOW_SIZE       100
#define PRESENT_THRESH    0.15f
#define ABSENT_THRESH     0.05f

static float window[WINDOW_SIZE];
static int   wpos, wfull;
static bool  last_published;

static float amplitude(const csi_frame_t *f)
{
    float sum = 0.0f;
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > CSI_MAX_SUBCARRIERS)
        n = CSI_MAX_SUBCARRIERS;
    for (int i = 0; i < n; i++)
        sum += sqrtf((float)f->data_i[i] * f->data_i[i] +
                     (float)f->data_q[i] * f->data_q[i]);
    return sum / n;
}

void presence_init(void)
{
    memset(window, 0, sizeof(window));
    wpos = wfull = 0;
    last_published = false;
}

void presence_process(const csi_frame_t *frame)
{
    window[wpos] = amplitude(frame);
    wpos = (wpos + 1) % WINDOW_SIZE;
    if (!wfull && wpos == 0)
        wfull = 1;

    int n = wfull ? WINDOW_SIZE : wpos;
    if (n < 10)
        return;

    float mean = 0.0f;
    for (int i = 0; i < n; i++)
        mean += window[i];
    mean /= n;

    float var = 0.0f;
    for (int i = 0; i < n; i++) {
        float d = window[i] - mean;
        var += d * d;
    }
    var /= n;
    float norm_var = var / (mean * mean + 1e-6f);

    bool present;
    float confidence;
    if (norm_var > PRESENT_THRESH) {
        present = true;
        confidence = fminf(1.0f, norm_var / (PRESENT_THRESH * 3));
    } else if (norm_var < ABSENT_THRESH) {
        present = false;
        confidence = fminf(1.0f, (ABSENT_THRESH - norm_var) / ABSENT_THRESH);
    } else {
        return; /* hysteresis zone: hold last state */
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.present = present;
    g_state.presence_confidence = confidence;
    g_state.person_count = present ? 1 : 0;
    pthread_mutex_unlock(&g_state.lock);

    if (present != last_published) {
        char json[128];
        snprintf(json, sizeof(json),
            "{\"present\":%s,\"confidence\":%.2f,\"count\":%d}",
            present ? "true" : "false", confidence, present ? 1 : 0);
        mqtt_publish_event("presence", json);
        last_published = present;
    }
}

void presence_deinit(void) {}
