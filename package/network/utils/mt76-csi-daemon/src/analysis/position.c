#include <math.h>
#include <string.h>
#include <stdio.h>
#include "position.h"
#include "../state.h"
#include "../mqtt.h"

/* Zone estimation from mean subcarrier phase gradient across the band.
 * With 2 Rx antennas absolute positioning is impossible; we bin the
 * dominant phase-slope signature into the configured zones and gate on
 * amplitude variance (motion) so an empty room reports no zone. */

#define WINDOW_SIZE 64

static float amp_win[WINDOW_SIZE];
static int   wpos, wfull;
static int   last_zone = -1;

static void metrics(const csi_frame_t *f, float *amp, float *slope)
{
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > CSI_MAX_SUBCARRIERS)
        n = CSI_MAX_SUBCARRIERS;

    float a = 0.0f, sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        float mag = sqrtf((float)f->data_i[i] * f->data_i[i] +
                          (float)f->data_q[i] * f->data_q[i]);
        a += mag;
        if (f->data_i[i] || f->data_q[i]) {
            float ph = atan2f((float)f->data_q[i], (float)f->data_i[i]);
            sx += i; sy += ph; sxx += (float)i * i; sxy += (float)i * ph;
            cnt++;
        }
    }
    *amp = a / n;
    if (cnt > 2) {
        float denom = cnt * sxx - sx * sx;
        *slope = denom != 0.0f ? (cnt * sxy - sx * sy) / denom : 0.0f;
    } else {
        *slope = 0.0f;
    }
}

void position_init(void)
{
    memset(amp_win, 0, sizeof(amp_win));
    wpos = wfull = 0;
    last_zone = -1;
}

void position_process(const csi_frame_t *frame)
{
    float amp, slope;
    metrics(frame, &amp, &slope);

    amp_win[wpos] = amp;
    wpos = (wpos + 1) % WINDOW_SIZE;
    if (!wfull && wpos == 0)
        wfull = 1;

    int n = wfull ? WINDOW_SIZE : wpos;
    if (n < 10)
        return;

    float mean = 0.0f;
    for (int i = 0; i < n; i++)
        mean += amp_win[i];
    mean /= n;
    float var = 0.0f;
    for (int i = 0; i < n; i++) {
        float d = amp_win[i] - mean;
        var += d * d;
    }
    var /= n;
    float norm_var = var / (mean * mean + 1e-6f);

    int zones = g_config.zone_count > 0 ? g_config.zone_count : 1;
    int zone = -1;
    float conf = 0.0f;

    if (norm_var > 0.05f) {
        /* map phase slope [-pi,pi] -> zone bucket */
        float t = (slope + (float)M_PI) / (2.0f * (float)M_PI);
        if (t < 0.0f) t = 0.0f;
        if (t > 0.999f) t = 0.999f;
        zone = (int)(t * zones);
        if (zone >= zones)
            zone = zones - 1;
        conf = fminf(1.0f, norm_var / 0.15f);
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.active_zone = zone;
    g_state.zone_confidence = conf;
    for (int i = 0; i < CSI_MAX_ZONES; i++)
        g_state.zone_occupied[i] = (i == zone);
    pthread_mutex_unlock(&g_state.lock);

    if (zone != last_zone) {
        char json[96];
        snprintf(json, sizeof(json),
            "{\"zone\":%d,\"confidence\":%.2f}", zone, conf);
        mqtt_publish_event("position", json);
        last_zone = zone;
    }
}

void position_deinit(void) {}
