/*
 * Vitals accuracy harness.
 *
 * Replays synthetic CSI frames with known respiration and cardiac frequencies
 * through the real vitals_process() and scores the reported bpm against truth.
 *
 * Usage:
 *   ./test_vitals              full sweep (rate x snr x jitter x drift)
 *   ./test_vitals --seeds N    trials per config (default 20)
 *   ./test_vitals --quick      reduced grid for fast iteration
 *   ./test_vitals --sanity     one verbose trial, nominal conditions
 *
 * A trial feeds HARNESS_FRAMES frames. That is a fixed frame count, not a
 * fixed duration, because the daemon's window is a frame ring buffer: at 10 Hz
 * 600 frames span 60 s, at 30 Hz they span 20 s. Keep HARNESS_FRAMES constant
 * across baseline and candidate runs or the numbers are not comparable.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"
#include "stubs.h"
#include "../src/analysis/vitals.h"

#define HARNESS_FRAMES   600

/* A result counts as a hit if it lands within these tolerances. */
#define RESP_TOL_BRPM    2.0
#define HR_TOL_BPM       5.0

#define MAX_REASONS      8

typedef struct {
    int    trials;
    int    produced;
    double resp_err_sum;
    double hr_err_sum;
    double resp_err_max;
    double hr_err_max;
    int    resp_hits;
    int    hr_hits;
    char   reason[MAX_REASONS][32];
    int    reason_count[MAX_REASONS];
    int    reasons;
} score_t;

static void score_reset(score_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void score_reason(score_t *s, const char *why)
{
    for (int i = 0; i < s->reasons; i++) {
        if (strcmp(s->reason[i], why) == 0) {
            s->reason_count[i]++;
            return;
        }
    }
    if (s->reasons >= MAX_REASONS)
        return;
    snprintf(s->reason[s->reasons], 32, "%s", why);
    s->reason_count[s->reasons]++;
    s->reasons++;
}

static void score_add(score_t *s, int feasible, const char *why,
                      double resp_err, double hr_err)
{
    s->trials++;
    if (!feasible) {
        score_reason(s, why && why[0] ? why : "none");
        return;
    }
    s->produced++;
    s->resp_err_sum += resp_err;
    s->hr_err_sum   += hr_err;
    if (resp_err > s->resp_err_max)
        s->resp_err_max = resp_err;
    if (hr_err > s->hr_err_max)
        s->hr_err_max = hr_err;
    if (resp_err <= RESP_TOL_BRPM)
        s->resp_hits++;
    if (hr_err <= HR_TOL_BPM)
        s->hr_hits++;
}

static void score_merge(score_t *dst, const score_t *src)
{
    dst->trials   += src->trials;
    dst->produced += src->produced;
    dst->resp_err_sum += src->resp_err_sum;
    dst->hr_err_sum   += src->hr_err_sum;
    dst->resp_hits += src->resp_hits;
    dst->hr_hits   += src->hr_hits;
    if (src->resp_err_max > dst->resp_err_max)
        dst->resp_err_max = src->resp_err_max;
    if (src->hr_err_max > dst->hr_err_max)
        dst->hr_err_max = src->hr_err_max;
    for (int i = 0; i < src->reasons; i++) {
        for (int j = 0; j < src->reason_count[i]; j++)
            score_reason(dst, src->reason[i]);
    }
}

static double mae_resp(const score_t *s)
{
    return s->produced ? s->resp_err_sum / s->produced : NAN;
}

static double mae_hr(const score_t *s)
{
    return s->produced ? s->hr_err_sum / s->produced : NAN;
}

/* Hit rates are over ALL trials: refusing to answer is a failure, not a pass. */
static double hit_resp(const score_t *s)
{
    return s->trials ? 100.0 * s->resp_hits / s->trials : 0.0;
}

static double hit_hr(const score_t *s)
{
    return s->trials ? 100.0 * s->hr_hits / s->trials : 0.0;
}

static double yield_pct(const score_t *s)
{
    return s->trials ? 100.0 * s->produced / s->trials : 0.0;
}

/*
 * Confidence calibration. A confidence number is only worth reporting if it
 * predicts accuracy, so trials are bucketed by the confidence the code emitted
 * and the hit rate is reported per bucket. A useful confidence shows hit rate
 * rising with the bucket; a flat curve means the number carries no information.
 */
#define CONF_BUCKETS 5
static int conf_n[CONF_BUCKETS];
static int conf_hit_r[CONF_BUCKETS];
static int conf_hit_h[CONF_BUCKETS];

static void conf_record(float conf, int resp_ok, int hr_ok)
{
    int b = (int)(conf * CONF_BUCKETS);
    if (b < 0)
        b = 0;
    if (b >= CONF_BUCKETS)
        b = CONF_BUCKETS - 1;
    conf_n[b]++;
    conf_hit_r[b] += resp_ok;
    conf_hit_h[b] += hr_ok;
}

/* Ground truth depends only on the seed, so the same seed means the same
 * person across every config in the sweep. */
static void truth_for_seed(unsigned seed, float *resp_hz, float *hr_hz)
{
    rng_t r;
    rng_seed(&r, 0x5EED0000u + seed);
    *resp_hz = rng_range(&r, 0.1667f, 0.3333f);   /* 10 - 20 brpm */
    *hr_hz   = rng_range(&r, 0.9167f, 1.5000f);   /* 55 - 90 bpm  */
}

typedef struct {
    int    feasible;
    char   reason[32];
    int    resp_bpm;
    int    hr_bpm;
    float  confidence;
    double elapsed_s;
} trial_result_t;

static void run_trial(const synth_cfg_t *cfg, trial_result_t *res)
{
    synth_state_t *sy = synth_create(cfg);
    csi_frame_t frame;

    stub_reset();
    /* Feasibility preconditions the vitals path gates on. */
    g_state.vitals_zone = 0;
    g_state.present = true;
    g_state.zone_occupied[0] = true;

    vitals_init();
    for (int i = 0; i < HARNESS_FRAMES; i++) {
        synth_next(sy, &frame);
        vitals_process(&frame);
    }

    memset(res, 0, sizeof(*res));
    res->feasible   = g_state.vitals_feasible;
    res->resp_bpm   = g_state.respiration_bpm;
    res->hr_bpm     = g_state.heart_rate_bpm;
    res->confidence = g_state.vitals_confidence;
    res->elapsed_s  = synth_elapsed_s(sy);
    snprintf(res->reason, sizeof(res->reason), "%s", g_state.vitals_reason);

    synth_destroy(sy);
}

static void run_config(const synth_cfg_t *base, int seeds, score_t *out)
{
    score_reset(out);
    for (int s = 0; s < seeds; s++) {
        synth_cfg_t cfg = *base;
        cfg.seed = (unsigned)(s + 1);
        truth_for_seed(cfg.seed, &cfg.resp_hz, &cfg.hr_hz);

        trial_result_t r;
        run_trial(&cfg, &r);

        double resp_truth = cfg.resp_hz * 60.0;
        double hr_truth   = cfg.hr_hz * 60.0;
        double resp_err = fabs(r.resp_bpm - resp_truth);
        double hr_err   = fabs(r.hr_bpm - hr_truth);
        score_add(out, r.feasible, r.reason, resp_err, hr_err);

        if (r.feasible)
            conf_record(r.confidence,
                        resp_err <= RESP_TOL_BRPM, hr_err <= HR_TOL_BPM);
    }
}

static void print_score_row(const char *label, const score_t *s)
{
    printf("%-26s %5d %6.1f%% %8.2f %8.2f %7.1f%% %7.1f%%\n",
           label, s->trials, yield_pct(s),
           s->produced ? mae_resp(s) : 0.0,
           s->produced ? mae_hr(s) : 0.0,
           hit_resp(s), hit_hr(s));
}

static void print_header(const char *title)
{
    printf("\n%s\n", title);
    printf("%-26s %5s %7s %8s %8s %8s %8s\n",
           "config", "n", "yield", "MAEresp", "MAEhr", "hit_r", "hit_h");
    printf("--------------------------------------------------------------------------------\n");
}

static void sanity(void)
{
    synth_cfg_t cfg;
    synth_defaults(&cfg);
    cfg.seed = 1;
    truth_for_seed(cfg.seed, &cfg.resp_hz, &cfg.hr_hz);

    trial_result_t r;
    run_trial(&cfg, &r);

    printf("sanity trial (nominal: %.0f Hz, SNR %.0f dB, no jitter, no drift)\n",
           (double)cfg.rate_hz, (double)cfg.snr_db);
    printf("  frames fed      : %d over %.1f s\n", HARNESS_FRAMES, r.elapsed_s);
    printf("  truth resp      : %.2f brpm  (%.4f Hz)\n",
           cfg.resp_hz * 60.0, (double)cfg.resp_hz);
    printf("  truth hr        : %.2f bpm   (%.4f Hz)\n",
           cfg.hr_hz * 60.0, (double)cfg.hr_hz);
    printf("  feasible        : %s%s%s\n", r.feasible ? "yes" : "no",
           r.feasible ? "" : " reason=", r.feasible ? "" : r.reason);
    printf("  reported resp   : %d brpm  (err %.2f)\n",
           r.resp_bpm, fabs(r.resp_bpm - cfg.resp_hz * 60.0));
    printf("  reported hr     : %d bpm   (err %.2f)\n",
           r.hr_bpm, fabs(r.hr_bpm - cfg.hr_hz * 60.0));
    printf("  confidence      : %.3f\n", (double)r.confidence);
}

int main(int argc, char **argv)
{
    int seeds = 20;
    int quick = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sanity") == 0) {
            sanity();
            return 0;
        } else if (strcmp(argv[i], "--quick") == 0) {
            quick = 1;
        } else if (strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
            seeds = atoi(argv[++i]);
            if (seeds < 1)
                seeds = 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    static const float rates_full[]  = { 10.0f, 15.0f, 20.0f, 25.0f, 30.0f };
    static const float snrs_full[]   = { 30.0f, 20.0f, 12.0f };
    static const float jitter_full[] = { 0.0f, 0.3f };
    static const float drift_full[]  = { 0.0f, 0.10f };

    static const float rates_quick[]  = { 10.0f, 20.0f, 30.0f };
    static const float snrs_quick[]   = { 30.0f, 15.0f };
    static const float jitter_quick[] = { 0.0f, 0.3f };
    static const float drift_quick[]  = { 0.0f, 0.10f };

    const float *rates  = quick ? rates_quick  : rates_full;
    const float *snrs   = quick ? snrs_quick   : snrs_full;
    const float *jits   = quick ? jitter_quick : jitter_full;
    const float *drifts = quick ? drift_quick  : drift_full;
    int n_rates  = quick ? 3 : 5;
    int n_snrs   = quick ? 2 : 3;
    int n_jits   = 2;
    int n_drifts = 2;

    score_t overall;
    score_reset(&overall);

    score_t by_rate[8], by_snr[8], by_jit[4], by_drift[4];
    for (int i = 0; i < 8; i++) {
        score_reset(&by_rate[i]);
        score_reset(&by_snr[i]);
    }
    for (int i = 0; i < 4; i++) {
        score_reset(&by_jit[i]);
        score_reset(&by_drift[i]);
    }

    printf("vitals harness: %d seeds/config, %d frames/trial\n", seeds, HARNESS_FRAMES);
    printf("tolerances: resp +/-%.1f brpm, hr +/-%.1f bpm\n", RESP_TOL_BRPM, HR_TOL_BPM);
    printf("hit rates are over ALL trials (a refusal counts as a miss)\n");

    print_header("per-config results");

    for (int ri = 0; ri < n_rates; ri++) {
        for (int si = 0; si < n_snrs; si++) {
            for (int ji = 0; ji < n_jits; ji++) {
                for (int di = 0; di < n_drifts; di++) {
                    synth_cfg_t cfg;
                    synth_defaults(&cfg);
                    cfg.rate_hz     = rates[ri];
                    cfg.snr_db      = snrs[si];
                    cfg.jitter_frac = jits[ji];
                    cfg.drift_frac  = drifts[di];

                    score_t s;
                    run_config(&cfg, seeds, &s);

                    char label[32];
                    snprintf(label, sizeof(label), "r%.0f s%.0f j%.1f d%.2f",
                             (double)cfg.rate_hz, (double)cfg.snr_db,
                             (double)cfg.jitter_frac, (double)cfg.drift_frac);
                    print_score_row(label, &s);

                    score_merge(&overall, &s);
                    score_merge(&by_rate[ri], &s);
                    score_merge(&by_snr[si], &s);
                    score_merge(&by_jit[ji], &s);
                    score_merge(&by_drift[di], &s);
                }
            }
        }
    }

    print_header("by frame rate");
    for (int i = 0; i < n_rates; i++) {
        char label[32];
        snprintf(label, sizeof(label), "rate %.0f Hz", (double)rates[i]);
        print_score_row(label, &by_rate[i]);
    }

    print_header("by SNR");
    for (int i = 0; i < n_snrs; i++) {
        char label[32];
        snprintf(label, sizeof(label), "snr %.0f dB", (double)snrs[i]);
        print_score_row(label, &by_snr[i]);
    }

    print_header("by timing jitter");
    for (int i = 0; i < n_jits; i++) {
        char label[32];
        snprintf(label, sizeof(label), "jitter %.1f", (double)jits[i]);
        print_score_row(label, &by_jit[i]);
    }

    print_header("by gain drift");
    for (int i = 0; i < n_drifts; i++) {
        char label[32];
        snprintf(label, sizeof(label), "drift %.2f", (double)drifts[i]);
        print_score_row(label, &by_drift[i]);
    }

    print_header("OVERALL");
    print_score_row("all configs", &overall);

    print_header("confidence calibration (does reported confidence predict accuracy?)");
    printf("%-26s %5s %7s %8s %8s\n", "confidence bucket", "n", "", "hit_r", "hit_h");
    printf("--------------------------------------------------------------------------------\n");
    for (int b = 0; b < CONF_BUCKETS; b++) {
        char label[32];
        snprintf(label, sizeof(label), "%.1f - %.1f",
                 (double)b / CONF_BUCKETS, (double)(b + 1) / CONF_BUCKETS);
        if (!conf_n[b]) {
            printf("%-26s %5d %7s %8s %8s\n", label, 0, "-", "-", "-");
            continue;
        }
        printf("%-26s %5d %7s %7.1f%% %7.1f%%\n", label, conf_n[b], "",
               100.0 * conf_hit_r[b] / conf_n[b],
               100.0 * conf_hit_h[b] / conf_n[b]);
    }

    printf("\nrefusal reasons (%d of %d trials produced no estimate)\n",
           overall.trials - overall.produced, overall.trials);
    for (int i = 0; i < overall.reasons; i++)
        printf("  %-24s %d\n", overall.reason[i], overall.reason_count[i]);

    printf("\nSUMMARY yield=%.1f%% MAEresp=%.2f MAEhr=%.2f hitresp=%.1f%% hithr=%.1f%%\n",
           yield_pct(&overall), mae_resp(&overall), mae_hr(&overall),
           hit_resp(&overall), hit_hr(&overall));
    return 0;
}
