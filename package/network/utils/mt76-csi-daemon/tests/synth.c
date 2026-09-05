#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"

#define C_LIGHT_MM_S   299792458000.0   /* speed of light, mm/s */
#define FC_HZ          5.2e9            /* carrier */
#define SUB_SPACING_HZ 312500.0         /* 802.11 subcarrier spacing */
#define STATIC_MAG     1800.0f          /* nominal |a_k| in int16 units */

struct synth_state {
    synth_cfg_t cfg;
    rng_t       rng;

    float   a_re[CSI_MAX_SUBCARRIERS];
    float   a_im[CSI_MAX_SUBCARRIERS];
    float   b_mag[CSI_MAX_SUBCARRIERS];
    float   b_phase[CSI_MAX_SUBCARRIERS];
    float   k_wave[CSI_MAX_SUBCARRIERS];  /* 4*pi/lambda_k, rad per mm */

    float   noise_sigma;
    float   resp_phase0;
    float   hr_phase0;
    float   drift_phase0;

    double  t_s;          /* signal time of the next frame */
    double  ts_ms_acc;    /* accumulated timestamp, ms */
    float   body_walk;    /* random-walk body displacement state */
};

void rng_seed(rng_t *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ull;
}

static uint64_t rng_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

float rng_uniform(rng_t *r)
{
    return (float)((rng_next(r) >> 40) / 16777216.0);
}

float rng_range(rng_t *r, float lo, float hi)
{
    return lo + (hi - lo) * rng_uniform(r);
}

float rng_normal(rng_t *r)
{
    float u1 = rng_uniform(r);
    float u2 = rng_uniform(r);
    if (u1 < 1e-7f)
        u1 = 1e-7f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

void synth_defaults(synth_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->resp_hz      = 0.25f;   /* 15 brpm */
    cfg->hr_hz        = 1.20f;   /* 72 bpm */
    cfg->resp_amp_mm  = 5.0f;
    cfg->hr_amp_mm    = 0.5f;
    cfg->reflect_frac = 0.10f;
    cfg->rate_hz      = 20.0f;
    cfg->jitter_frac  = 0.0f;
    cfg->snr_db       = 30.0f;
    cfg->drift_frac   = 0.0f;
    cfg->drift_hz     = 0.02f;
    cfg->n_sub        = 64;
    cfg->ts_start_ms  = 1000;
    cfg->body_motion_mm = 0.0f;
    cfg->seed         = 1;
}

synth_state_t *synth_create(const synth_cfg_t *cfg)
{
    synth_state_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->cfg = *cfg;
    if (s->cfg.n_sub <= 0 || s->cfg.n_sub > CSI_MAX_SUBCARRIERS)
        s->cfg.n_sub = 64;
    rng_seed(&s->rng, s->cfg.seed);

    int n = s->cfg.n_sub;
    for (int k = 0; k < n; k++) {
        /* Static multipath: magnitude spread plus uniform phase. */
        float mag   = STATIC_MAG * rng_range(&s->rng, 0.55f, 1.45f);
        float phase = rng_range(&s->rng, -(float)M_PI, (float)M_PI);
        s->a_re[k] = mag * cosf(phase);
        s->a_im[k] = mag * sinf(phase);

        /* Chest reflection: weak, with its own path phase offset. */
        s->b_mag[k]   = mag * s->cfg.reflect_frac * rng_range(&s->rng, 0.6f, 1.4f);
        s->b_phase[k] = rng_range(&s->rng, -(float)M_PI, (float)M_PI);

        double f_k    = FC_HZ + (k - n / 2) * SUB_SPACING_HZ;
        double lambda = C_LIGHT_MM_S / f_k;
        s->k_wave[k]  = (float)(4.0 * M_PI / lambda);
    }

    /* Noise sigma per I/Q component from the mean static magnitude. */
    float snr_lin = powf(10.0f, s->cfg.snr_db / 10.0f);
    s->noise_sigma = STATIC_MAG / sqrtf(2.0f * snr_lin);

    s->resp_phase0  = rng_range(&s->rng, -(float)M_PI, (float)M_PI);
    s->hr_phase0    = rng_range(&s->rng, -(float)M_PI, (float)M_PI);
    s->drift_phase0 = rng_range(&s->rng, -(float)M_PI, (float)M_PI);

    s->t_s = 0.0;
    s->ts_ms_acc = (double)s->cfg.ts_start_ms;
    s->body_walk = 0.0f;
    return s;
}

void synth_destroy(synth_state_t *s)
{
    free(s);
}

double synth_elapsed_s(const synth_state_t *s)
{
    return s->t_s;
}

void synth_set_body_motion(synth_state_t *s, float mm)
{
    s->cfg.body_motion_mm = mm;
}

static int16_t clamp16(float v)
{
    if (v > 32767.0f)
        return 32767;
    if (v < -32768.0f)
        return -32768;
    return (int16_t)lrintf(v);
}

void synth_next(synth_state_t *s, csi_frame_t *out)
{
    const synth_cfg_t *c = &s->cfg;
    int n = c->n_sub;
    double t = s->t_s;

    /* Chest displacement in mm. */
    float d = c->resp_amp_mm * sinf(2.0f * (float)M_PI * c->resp_hz * (float)t + s->resp_phase0)
            + c->hr_amp_mm   * sinf(2.0f * (float)M_PI * c->hr_hz   * (float)t + s->hr_phase0);

    /* Optional gross body motion: low-pass random walk, mm rms. */
    if (c->body_motion_mm > 0.0f) {
        s->body_walk = 0.92f * s->body_walk + 0.39f * c->body_motion_mm * rng_normal(&s->rng);
        d += s->body_walk;
    }

    /* Slow gain drift. */
    float g = 1.0f;
    if (c->drift_frac != 0.0f)
        g += c->drift_frac * sinf(2.0f * (float)M_PI * c->drift_hz * (float)t + s->drift_phase0);

    memset(out, 0, sizeof(*out));
    out->ts       = (uint32_t)fmod(s->ts_ms_acc, 4294967296.0);
    out->data_num = (uint16_t)n;
    out->data_bw  = 20;
    out->rssi     = -45;
    out->snr      = (uint8_t)(c->snr_db > 0 ? c->snr_db : 0);

    for (int k = 0; k < n; k++) {
        float ang = s->b_phase[k] - s->k_wave[k] * d;
        float re = s->a_re[k] + s->b_mag[k] * cosf(ang);
        float im = s->a_im[k] + s->b_mag[k] * sinf(ang);

        re *= g;
        im *= g;

        if (s->noise_sigma > 0.0f) {
            re += s->noise_sigma * rng_normal(&s->rng);
            im += s->noise_sigma * rng_normal(&s->rng);
        }

        out->data_i[k] = clamp16(re);
        out->data_q[k] = clamp16(im);
    }

    /* Advance the clock with jitter. */
    double interval_ms = 1000.0 / (c->rate_hz > 0.1f ? c->rate_hz : 0.1f);
    double jit = 0.0;
    if (c->jitter_frac > 0.0f)
        jit = interval_ms * c->jitter_frac * (2.0 * rng_uniform(&s->rng) - 1.0);
    double step = interval_ms + jit;
    if (step < 0.1)
        step = 0.1;

    s->ts_ms_acc += step;
    s->t_s += step / 1000.0;
}
