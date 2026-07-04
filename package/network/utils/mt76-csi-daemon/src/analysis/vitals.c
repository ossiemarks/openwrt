#include <math.h>
#include <string.h>
#include <stdio.h>
#include "vitals.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Vital signs via band-limited DFT on the CSI amplitude time series.
 * Frame rate depends on traffic; ~20 fps assumed. A 30s window is used.
 *
 * Respiration: 0.1 – 0.5 Hz   Heart rate: 0.8 – 2.0 Hz
 *
 * Feasibility gates: a zone must be focused (vitals_zone >= 0), presence
 * true, the focused zone occupied, and recent motion low (person still).
 */

#define VITALS_SAMPLES  600
#define SAMPLE_RATE_HZ  20.0f

static float amp_buf[VITALS_SAMPLES];
static int   buf_pos, buf_full, frame_cnt;

static float dft_magnitude(const float *x, int n, float freq_hz)
{
    float re = 0, im = 0;
    float w = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE_HZ;
    for (int k = 0; k < n; k++) {
        re += x[k] * cosf(w * k);
        im -= x[k] * sinf(w * k);
    }
    return sqrtf(re * re + im * im) / n;
}

static float peak_freq(const float *x, int n, float f_lo, float f_hi)
{
    float best_mag = -1, best_f = 0;
    for (float f = f_lo; f <= f_hi; f += 0.02f) {
        float m = dft_magnitude(x, n, f);
        if (m > best_mag) {
            best_mag = m;
            best_f = f;
        }
    }
    return best_f;
}

static float amplitude(const csi_frame_t *f)
{
    float s = 0.0f;
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > CSI_MAX_SUBCARRIERS)
        n = CSI_MAX_SUBCARRIERS;
    for (int i = 0; i < n; i++)
        s += sqrtf((float)f->data_i[i] * f->data_i[i] +
                   (float)f->data_q[i] * f->data_q[i]);
    return s / n;
}

static bool set_infeasible(const char *reason)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(g_state.vitals_reason, sizeof(g_state.vitals_reason), "%s", reason);
    g_state.vitals_feasible = false;
    pthread_mutex_unlock(&g_state.lock);
    return false;
}

static bool check_feasibility(void)
{
    pthread_mutex_lock(&g_state.lock);
    int fz = g_state.vitals_zone;
    bool present = g_state.present;
    bool occ = (fz >= 0 && fz < CSI_MAX_ZONES) ? g_state.zone_occupied[fz] : false;
    pthread_mutex_unlock(&g_state.lock);

    if (fz < 0)
        return set_infeasible("no_zone_focused");
    if (!present)
        return set_infeasible("low_presence_confidence");
    if (!occ)
        return set_infeasible("low_presence_confidence");
    return true;
}

static bool motion_detected(void)
{
    int n = buf_full ? VITALS_SAMPLES : buf_pos;
    if (n < 20)
        return false;
    float mean = 0;
    for (int i = 0; i < n; i++)
        mean += amp_buf[i];
    mean /= n;
    float var = 0;
    for (int i = 0; i < n; i++) {
        float d = amp_buf[i] - mean;
        var += d * d;
    }
    var /= n;
    return var / (mean * mean + 1e-6f) > 0.30f;
}

void vitals_init(void)
{
    memset(amp_buf, 0, sizeof(amp_buf));
    buf_pos = buf_full = frame_cnt = 0;
}

void vitals_process(const csi_frame_t *frame)
{
    amp_buf[buf_pos] = amplitude(frame);
    buf_pos = (buf_pos + 1) % VITALS_SAMPLES;
    if (!buf_full && buf_pos == 0)
        buf_full = 1;

    frame_cnt++;
    if (frame_cnt % (VITALS_SAMPLES / 2) != 0)
        return; /* analyse ~every 15s */

    if (!check_feasibility())
        return;

    if (motion_detected()) {
        set_infeasible("motion_detected");
        return;
    }

    int n = buf_full ? VITALS_SAMPLES : buf_pos;
    static float linear[VITALS_SAMPLES];
    int start = buf_full ? buf_pos : 0;
    for (int i = 0; i < n; i++)
        linear[i] = amp_buf[(start + i) % VITALS_SAMPLES];

    float resp_bpm = peak_freq(linear, n, 0.1f, 0.5f) * 60.0f;
    float hr_bpm   = peak_freq(linear, n, 0.8f, 2.0f) * 60.0f;

    if (resp_bpm < 6.0f || resp_bpm > 30.0f ||
        hr_bpm < 40.0f || hr_bpm > 120.0f) {
        set_infeasible("low_snr");
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.vitals_feasible = true;
    g_state.respiration_bpm = (int)(resp_bpm + 0.5f);
    g_state.heart_rate_bpm = (int)(hr_bpm + 0.5f);
    g_state.vitals_confidence = 0.75f;
    g_state.vitals_reason[0] = '\0';
    int zone = g_state.vitals_zone;
    pthread_mutex_unlock(&g_state.lock);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"zone\":%d,\"feasible\":true,\"respiration_bpm\":%d,"
        "\"heart_rate_bpm\":%d,\"confidence\":0.75,\"window_seconds\":30}",
        zone, (int)(resp_bpm + 0.5f), (int)(hr_bpm + 0.5f));
    mqtt_publish_event("vitals", json);
}

void vitals_deinit(void) {}
