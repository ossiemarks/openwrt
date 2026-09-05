#ifndef CSI_SYNTH_H
#define CSI_SYNTH_H

#include <stdint.h>
#include "../src/csi_frame.h"

/*
 * Synthetic CSI generator with known ground truth.
 *
 * Physical model, per subcarrier k:
 *
 *   h_k(t) = g(t) * [ a_k + b_k * exp(-j * 4*pi*d(t) / lambda_k) ] + n_k(t)
 *
 *   a_k      static multipath (random magnitude/phase, fixed per run)
 *   b_k      reflection off the moving chest (weak relative to a_k)
 *   d(t)     chest displacement:
 *              A_resp * sin(2*pi*f_resp*t + p_r) + A_hr * sin(2*pi*f_hr*t + p_h)
 *   lambda_k wavelength of subcarrier k (round trip -> factor 4*pi)
 *   g(t)     slow gain drift (AGC / thermal), 1 + drift*sin(2*pi*f_drift*t + p_d)
 *   n_k(t)   complex AWGN set from snr_db
 *
 * Because a_k has a random phase per subcarrier, the operating point on the
 * |a_k + b_k*e^{jx}| curve differs per subcarrier: some subcarriers carry the
 * displacement signal strongly, others barely at all. Averaging |h_k| over all
 * subcarriers therefore attenuates the vitals signal, which is the behaviour a
 * real deployment shows.
 *
 * Frame timestamps advance by 1000/rate_hz ms with uniform jitter and are
 * written into csi_frame_t.ts as the firmware does (uint32 ms, wraps).
 */

typedef struct {
    /* ground truth */
    float resp_hz;          /* respiration frequency */
    float hr_hz;            /* cardiac frequency */

    /* signal strength */
    float resp_amp_mm;      /* chest displacement, respiration */
    float hr_amp_mm;        /* chest displacement, cardiac */
    float reflect_frac;     /* |b_k| / |a_k| mean */

    /* acquisition */
    float rate_hz;          /* nominal frame rate */
    float jitter_frac;      /* uniform timing jitter, fraction of interval */
    float snr_db;           /* per-subcarrier SNR */
    float drift_frac;       /* slow gain drift amplitude */
    float drift_hz;         /* slow gain drift frequency */
    int   n_sub;            /* subcarriers per frame */
    uint32_t ts_start_ms;   /* initial timestamp (set high to test wrap) */

    /* body motion: extra broadband displacement, mm rms (0 = person still) */
    float body_motion_mm;

    unsigned seed;
} synth_cfg_t;

typedef struct synth_state synth_state_t;

void synth_defaults(synth_cfg_t *cfg);

/* Allocates and seeds per-subcarrier static/dynamic path coefficients. */
synth_state_t *synth_create(const synth_cfg_t *cfg);
void synth_destroy(synth_state_t *s);

/* Fills the next frame and advances the internal clock. */
void synth_next(synth_state_t *s, csi_frame_t *out);

/* Overrides body_motion_mm mid-run so a test can shape a motion envelope. */
void synth_set_body_motion(synth_state_t *s, float mm);

/* Elapsed signal time in seconds since the first frame. */
double synth_elapsed_s(const synth_state_t *s);

/* Deterministic RNG shared with the test drivers so sweeps are reproducible. */
typedef struct { uint64_t s; } rng_t;
void  rng_seed(rng_t *r, uint64_t seed);
float rng_uniform(rng_t *r);          /* [0,1) */
float rng_range(rng_t *r, float lo, float hi);
float rng_normal(rng_t *r);           /* mean 0, sd 1 */

#endif /* CSI_SYNTH_H */
