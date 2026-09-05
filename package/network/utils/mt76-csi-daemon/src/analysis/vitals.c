#include <math.h>
#include <string.h>
#include <stdio.h>
#include "vitals.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Vital signs via band-limited DFT over a window of VITALS_SAMPLES frames.
 *
 * The sample rate is measured from the per-frame firmware timestamps rather
 * than assumed, so the window spans however long those frames actually took;
 * at a low frame rate the same buffer covers proportionally more time.
 *
 * The series analysed is a sign-aligned combination of the most responsive
 * subcarriers, not the mean amplitude across all of them, and it is detrended
 * before the transform.
 *
 * Respiration: 0.1 – 0.5 Hz   Heart rate: 0.8 – 2.0 Hz
 *
 * Feasibility gates: presence true, an occupied zone (vitals_zone follows
 * the zone presence reports), and recent motion low (person still).
 */

#define VITALS_SAMPLES  600

/* Only a fallback for when the firmware timestamps are unusable. */
#define SAMPLE_RATE_FALLBACK_HZ 20.0f
#define SAMPLE_RATE_MIN_HZ       2.0f
#define SAMPLE_RATE_MAX_HZ     500.0f

/* Inter-frame gaps beyond this are treated as a stall, not a sample period. */
#define MAX_FRAME_GAP_MS      10000u

/*
 * Subcarriers tracked individually for selection, and how many of them are
 * combined. Chest displacement moves every subcarrier's operating point on
 * the |a + b*e^{j*theta}| curve differently, so some subcarriers carry the
 * vitals modulation strongly and others barely at all, with the sign of the
 * modulation varying between them. Averaging all of them, as a plain mean
 * amplitude does, lets those contributions partially cancel.
 */
#define VITALS_TRACK_SUB  64
#define VITALS_COMBINE     8

static float    amp_buf[VITALS_SAMPLES];
static uint32_t ts_buf[VITALS_SAMPLES];
static float    sub_buf[VITALS_TRACK_SUB][VITALS_SAMPLES];
static int      sub_count;
static int      buf_pos, buf_full, frame_cnt;

/*
 * Effective frame rate from the firmware timestamps.
 *
 * The frame rate is set by how much traffic the monitored station happens to
 * be sending, so it is not a constant and every frequency estimate scales by
 * (true_rate / assumed_rate). Deltas are computed in uint32 so the firmware's
 * 32-bit millisecond counter wraps correctly; stalls and duplicate timestamps
 * are excluded rather than allowed to skew the mean.
 */
static float measure_rate(const uint32_t *ts, int n)
{
    if (n < 2)
        return SAMPLE_RATE_FALLBACK_HZ;

    double total_ms = 0.0;
    int    steps = 0;

    for (int i = 1; i < n; i++) {
        uint32_t d = ts[i] - ts[i - 1];   /* wraps correctly */
        if (d > 0 && d < MAX_FRAME_GAP_MS) {
            total_ms += d;
            steps++;
        }
    }

    if (steps < n / 2 || total_ms <= 0.0)
        return SAMPLE_RATE_FALLBACK_HZ;

    float rate = (float)(1000.0 * steps / total_ms);
    if (rate < SAMPLE_RATE_MIN_HZ || rate > SAMPLE_RATE_MAX_HZ)
        return SAMPLE_RATE_FALLBACK_HZ;

    return rate;
}

static float dft_magnitude(const float *x, int n, float freq_hz, float rate_hz)
{
    float re = 0, im = 0;
    float w = 2.0f * (float)M_PI * freq_hz / rate_hz;
    for (int k = 0; k < n; k++) {
        re += x[k] * cosf(w * k);
        im -= x[k] * sinf(w * k);
    }
    return sqrtf(re * re + im * im) / n;
}

/*
 * Remove mean and linear trend. Without this the DC term of the amplitude
 * series (order 1e3) leaks across the whole band and dominates the vitals
 * components (order 1e1), so the band argmax degenerates to the low edge of
 * the search range regardless of the actual signal.
 */
static void detrend(float *x, int n)
{
    if (n < 2)
        return;

    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        sx  += i;
        sy  += x[i];
        sxx += (double)i * i;
        sxy += (double)i * x[i];
    }

    double denom = (double)n * sxx - sx * sx;
    double slope = denom != 0.0 ? ((double)n * sxy - sx * sy) / denom : 0.0;
    double icept = (sy - slope * sx) / n;

    for (int i = 0; i < n; i++)
        x[i] -= (float)(icept + slope * i);
}

/*
 * Band argmax.
 *
 * best_mag / floor_mag report the winning magnitude and the median magnitude
 * of the searched bins, so the caller can judge how prominent the peak is.
 *
 * No harmonic suppression is applied here. Respiration harmonics do fall in
 * the heart-rate band, but excluding bins near them was measured to make
 * heart rate substantially worse, both before and after subcarrier selection:
 * the exclusion window needed to cover a harmonic also covers a large part of
 * the plausible heart-rate range, so it discarded correct answers more often
 * than it removed an interferer.
 */
static float peak_freq(const float *x, int n, float f_lo, float f_hi,
                       float rate_hz, float *best_mag, float *floor_mag)
{
    float mags[128];
    int   nm = 0;
    float top = -1.0f, top_f = 0.0f;

    for (float f = f_lo; f <= f_hi; f += 0.02f) {
        float m = dft_magnitude(x, n, f, rate_hz);
        if (nm < 128)
            mags[nm++] = m;
        if (m > top) {
            top = m;
            top_f = f;
        }
    }

    if (floor_mag) {
        /* Median of the band as a noise-floor estimate (insertion sort, n<=128). */
        for (int i = 1; i < nm; i++) {
            float v = mags[i];
            int j = i - 1;
            while (j >= 0 && mags[j] > v) {
                mags[j + 1] = mags[j];
                j--;
            }
            mags[j + 1] = v;
        }
        *floor_mag = nm ? mags[nm / 2] : 0.0f;
    }
    if (best_mag)
        *best_mag = top;

    return top_f;
}

/*
 * Confidence from peak prominence: how far the winning bin stands above the
 * median bin of the same band. A ratio of 1 means the "peak" is no better than
 * a typical bin, i.e. there is no discernible tone and the reported rate is
 * meaningless. This replaces a hard-coded constant that was reported as if it
 * were a measurement.
 */
static float prominence_conf(float peak, float floor_mag)
{
    if (peak <= 0.0f || floor_mag <= 0.0f)
        return 0.0f;

    float r = peak / floor_mag;
    if (r <= 1.0f)
        return 0.0f;

    float c = (r - 1.0f) / (r - 1.0f + 2.0f);
    return c > 1.0f ? 1.0f : c;
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

/* Linearises one tracked subcarrier's ring buffer and removes its trend. */
static void extract_sub(int t, float *dst, int n, int start)
{
    for (int i = 0; i < n; i++)
        dst[i] = sub_buf[t][(start + i) % VITALS_SAMPLES];
    detrend(dst, n);
}

static float series_var(const float *x, int n)
{
    double v = 0;
    for (int i = 0; i < n; i++)
        v += (double)x[i] * x[i];
    return (float)(v / n);   /* already detrended, so mean is ~0 */
}

/*
 * Combines the highest-variance subcarriers into one series.
 *
 * Selection picks the subcarriers whose operating point makes them most
 * sensitive to the displacement. Because a subcarrier can sit on either slope
 * of the amplitude curve, its modulation may be in phase or in antiphase with
 * the others, so each contribution is sign-aligned against the strongest one
 * before summing; adding them blind would reintroduce the cancellation this is
 * meant to avoid. Each is normalised to unit variance so one loud subcarrier
 * cannot dominate.
 *
 * Returns the number of subcarriers combined, or 0 if selection is not
 * possible and the caller should fall back to the mean-amplitude series.
 */
static int combine_subcarriers(float *out, int n, int start)
{
    if (sub_count < 2 || n < 8)
        return 0;

    static float tmp[VITALS_SAMPLES];
    static float ref[VITALS_SAMPLES];
    float var[VITALS_TRACK_SUB];
    int   used[VITALS_TRACK_SUB];

    for (int t = 0; t < sub_count; t++) {
        extract_sub(t, tmp, n, start);
        var[t] = series_var(tmp, n);
        used[t] = 0;
    }

    int want = sub_count < VITALS_COMBINE ? sub_count : VITALS_COMBINE;
    int combined = 0;

    for (int pick = 0; pick < want; pick++) {
        int best = -1;
        for (int t = 0; t < sub_count; t++) {
            if (!used[t] && var[t] > 0.0f && (best < 0 || var[t] > var[best]))
                best = t;
        }
        if (best < 0)
            break;
        used[best] = 1;

        extract_sub(best, tmp, n, start);
        float sd = sqrtf(var[best]);
        if (sd <= 0.0f)
            continue;

        if (combined == 0) {
            for (int i = 0; i < n; i++) {
                ref[i] = tmp[i] / sd;
                out[i] = ref[i];
            }
        } else {
            double dot = 0;
            for (int i = 0; i < n; i++)
                dot += (double)tmp[i] * ref[i];
            float sign = dot < 0 ? -1.0f : 1.0f;
            for (int i = 0; i < n; i++)
                out[i] += sign * tmp[i] / sd;
        }
        combined++;
    }

    if (combined < 2)
        return 0;

    for (int i = 0; i < n; i++)
        out[i] /= (float)combined;

    return combined;
}

static bool set_infeasible(const char *reason)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(g_state.vitals_reason, sizeof(g_state.vitals_reason), "%s", reason);
    g_state.vitals_feasible = false;
    pthread_mutex_unlock(&g_state.lock);
    return false;
}

/*
 * Vitals follow the occupied zone. The single-AP position estimate is a
 * coarse phase-slope bucket, so pinning vitals to one configured zone
 * would starve it whenever the estimate lands one bucket over; instead the
 * focus tracks wherever presence currently puts the person.
 */
static bool check_feasibility(void)
{
    pthread_mutex_lock(&g_state.lock);
    bool present = g_state.present;
    int fz = g_state.vitals_zone;
    int az = g_state.active_zone;
    bool occ = (fz >= 0 && fz < CSI_MAX_ZONES) ? g_state.zone_occupied[fz] : false;

    if (present && !occ && az >= 0 && az < CSI_MAX_ZONES &&
        g_state.zone_occupied[az]) {
        g_state.vitals_zone = az;
        fz = az;
        occ = true;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (!present)
        return set_infeasible("not_present");
    if (fz < 0)
        return set_infeasible("no_zone_occupied");
    if (!occ)
        return set_infeasible("zone_not_occupied");
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
    memset(ts_buf, 0, sizeof(ts_buf));
    memset(sub_buf, 0, sizeof(sub_buf));
    sub_count = 0;
    buf_pos = buf_full = frame_cnt = 0;
}

void vitals_process(const csi_frame_t *frame)
{
    amp_buf[buf_pos] = amplitude(frame);
    ts_buf[buf_pos]  = frame->ts;

    /* Keep per-subcarrier amplitudes so the analysis step can pick the ones
     * actually carrying the modulation. Wide channels are subsampled to bound
     * the buffer. */
    int nsub = frame->data_num > 0 ? frame->data_num : 64;
    if (nsub > CSI_MAX_SUBCARRIERS)
        nsub = CSI_MAX_SUBCARRIERS;
    int stride = (nsub + VITALS_TRACK_SUB - 1) / VITALS_TRACK_SUB;
    if (stride < 1)
        stride = 1;
    int t = 0;
    for (int i = 0; i < nsub && t < VITALS_TRACK_SUB; i += stride, t++) {
        float re = (float)frame->data_i[i];
        float im = (float)frame->data_q[i];
        sub_buf[t][buf_pos] = sqrtf(re * re + im * im);
    }
    sub_count = t;

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
    static float    linear[VITALS_SAMPLES];
    static uint32_t ts_lin[VITALS_SAMPLES];
    int start = buf_full ? buf_pos : 0;
    for (int i = 0; i < n; i++) {
        linear[i] = amp_buf[(start + i) % VITALS_SAMPLES];
        ts_lin[i] = ts_buf[(start + i) % VITALS_SAMPLES];
    }

    float rate_hz = measure_rate(ts_lin, n);

    detrend(linear, n);

    /* Prefer the sign-aligned combination of the most responsive subcarriers;
     * fall back to the mean amplitude if selection is not possible. */
    static float sig[VITALS_SAMPLES];
    const float *series = linear;
    if (combine_subcarriers(sig, n, start) >= 2)
        series = sig;

    float resp_mag = 0.0f, resp_floor = 0.0f;
    float hr_mag = 0.0f, hr_floor = 0.0f;

    float resp_hz = peak_freq(series, n, 0.1f, 0.5f, rate_hz,
                              &resp_mag, &resp_floor);
    float hr_hz   = peak_freq(series, n, 0.8f, 2.0f, rate_hz,
                              &hr_mag, &hr_floor);

    float resp_bpm = resp_hz * 60.0f;
    float hr_bpm   = hr_hz * 60.0f;

    if (resp_bpm < 6.0f || resp_bpm > 30.0f ||
        hr_bpm < 40.0f || hr_bpm > 120.0f) {
        set_infeasible("low_snr");
        return;
    }

    /* The pair is only as trustworthy as its weaker member. */
    float conf_resp = prominence_conf(resp_mag, resp_floor);
    float conf_hr   = prominence_conf(hr_mag, hr_floor);
    float conf = conf_resp < conf_hr ? conf_resp : conf_hr;

    pthread_mutex_lock(&g_state.lock);
    g_state.vitals_feasible = true;
    g_state.respiration_bpm = (int)(resp_bpm + 0.5f);
    g_state.heart_rate_bpm = (int)(hr_bpm + 0.5f);
    g_state.vitals_confidence = conf;
    g_state.vitals_reason[0] = '\0';
    int zone = g_state.vitals_zone;
    pthread_mutex_unlock(&g_state.lock);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"zone\":%d,\"feasible\":true,\"respiration_bpm\":%d,"
        "\"heart_rate_bpm\":%d,\"confidence\":%.2f,\"window_seconds\":%d}",
        zone, (int)(resp_bpm + 0.5f), (int)(hr_bpm + 0.5f),
        (double)conf, (int)(n / rate_hz + 0.5f));
    mqtt_publish_event("vitals", json);
}

void vitals_deinit(void) {}
