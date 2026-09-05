/*
 * Motion-path harness: presence, position gating, gesture shape conformance.
 *
 * Scope and honesty notes, because not all of this path is scoreable against
 * synthetic truth:
 *
 *  presence  SCOREABLE. Ground truth is whether a dynamic (person) path exists
 *            at all. An empty room is modelled as reflect_frac = 0, i.e. static
 *            multipath plus noise plus gain drift and nothing else. Reported as
 *            specificity (empty room called empty), and sensitivity split into
 *            a still breathing person and a moving person, because a detector
 *            that only sees moving people is a motion detector, not a presence
 *            detector.
 *
 *  position  NOT SCOREABLE for zone accuracy. position.c bins a phase slope
 *            into zones; mapping a slope to a real room zone needs antenna
 *            geometry and per-zone channel signatures this model does not have.
 *            Inventing a zone truth would only test the code against itself.
 *            One genuine property IS scored: an empty room must report zone -1.
 *
 *  gesture   SPEC CONFORMANCE ONLY, not accuracy. gesture.c defines its classes
 *            by burst shape (centre peak = wave, rising = push, falling = pull),
 *            so we drive those shapes and check the label. This tests the
 *            implementation against its own stated rule; it does not show the
 *            rule corresponds to real human gestures.
 *
 * The reflector strength sweep matters: the hard-coded thresholds in these
 * files are normalised-variance figures, and normalised variance depends
 * directly on how strongly the person perturbs the channel. Sweeping it shows
 * the operating range rather than tuning to one arbitrary point.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"
#include "stubs.h"
#include "../src/analysis/presence.h"
#include "../src/analysis/position.h"
#include "../src/analysis/gesture.h"

#define MOTION_FRAMES    240
#define GESTURE_FRAMES   200
#define PRESENCE_WARMUP  200

typedef enum {
    SCENE_EMPTY,    /* no person: no dynamic path */
    SCENE_STILL,    /* person present, breathing only */
    SCENE_MOVING    /* person present, gross body motion */
} scene_t;

static void scene_cfg(synth_cfg_t *cfg, scene_t sc, float reflect, float snr,
                      float drift, unsigned seed)
{
    synth_defaults(cfg);
    cfg->seed        = seed;
    cfg->snr_db      = snr;
    cfg->drift_frac  = drift;
    cfg->rate_hz     = 20.0f;

    switch (sc) {
    case SCENE_EMPTY:
        cfg->reflect_frac   = 0.0f;
        cfg->body_motion_mm = 0.0f;
        break;
    case SCENE_STILL:
        cfg->reflect_frac   = reflect;
        cfg->body_motion_mm = 0.0f;
        break;
    case SCENE_MOVING:
        cfg->reflect_frac   = reflect;
        cfg->body_motion_mm = 12.0f;
        break;
    }
}

/*
 * Replays a quiet (empty-room) learning period, then the target scenario, and
 * returns the final g_state.present.
 *
 * The learning period is part of the protocol rather than a favour to any one
 * implementation: the empty-room variance floor is noise-dominated and moves
 * by more than an order of magnitude with SNR, so no fixed absolute threshold
 * can separate occupied from empty across conditions. A detector has to see
 * the quiet channel to know what quiet looks like. Both the original and the
 * adaptive implementation are measured under this identical protocol.
 *
 * The warm-up uses the same seed, so the static multipath is the same room;
 * only the dynamic (person) path appears in phase two.
 */
static bool run_presence(const synth_cfg_t *cfg)
{
    csi_frame_t f;

    stub_reset();
    presence_init();

    synth_cfg_t quiet;
    scene_cfg(&quiet, SCENE_EMPTY, 0.0f, cfg->snr_db, cfg->drift_frac, cfg->seed);
    synth_state_t *sq = synth_create(&quiet);
    for (int i = 0; i < PRESENCE_WARMUP; i++) {
        synth_next(sq, &f);
        presence_process(&f);
    }
    synth_destroy(sq);

    synth_state_t *sy = synth_create(cfg);
    for (int i = 0; i < MOTION_FRAMES; i++) {
        synth_next(sy, &f);
        presence_process(&f);
    }
    bool present = g_state.present;
    synth_destroy(sy);
    return present;
}

/* Returns g_state.active_zone after replaying a scene through position_process. */
static int run_position(const synth_cfg_t *cfg)
{
    synth_state_t *sy = synth_create(cfg);
    csi_frame_t f;

    stub_reset();
    position_init();
    for (int i = 0; i < MOTION_FRAMES; i++) {
        synth_next(sy, &f);
        position_process(&f);
    }
    int zone = g_state.active_zone;
    synth_destroy(sy);
    return zone;
}

/* Envelope shapes for the gesture conformance check. */
typedef enum { ENV_CENTRE, ENV_RISING, ENV_FALLING } env_t;

static float envelope(env_t e, float u)   /* u in [0,1] over the burst */
{
    switch (e) {
    case ENV_CENTRE:  return sinf((float)M_PI * u);
    case ENV_RISING:  return u;
    case ENV_FALLING: return 1.0f - u;
    }
    return 0.0f;
}

static const char *run_gesture(env_t e, float reflect, float snr, unsigned seed)
{
    synth_cfg_t cfg;
    scene_cfg(&cfg, SCENE_STILL, reflect, snr, 0.0f, seed);
    synth_state_t *sy = synth_create(&cfg);
    csi_frame_t f;

    stub_reset();
    gesture_init();

    /* quiet lead-in so the running mean settles */
    int lead = 40;
    for (int i = 0; i < lead; i++) {
        synth_set_body_motion(sy, 0.0f);
        synth_next(sy, &f);
        gesture_process(&f);
    }
    /* shaped burst */
    int burst = GESTURE_FRAMES - 2 * lead;
    for (int i = 0; i < burst; i++) {
        float u = (float)i / (float)(burst - 1);
        synth_set_body_motion(sy, 25.0f * envelope(e, u));
        synth_next(sy, &f);
        gesture_process(&f);
    }
    /* quiet tail so the burst terminates and classify() runs */
    for (int i = 0; i < lead; i++) {
        synth_set_body_motion(sy, 0.0f);
        synth_next(sy, &f);
        gesture_process(&f);
    }

    static char label[24];
    snprintf(label, sizeof(label), "%s", g_state.last_gesture);
    synth_destroy(sy);
    return label;
}

/*
 * Reports the normalised amplitude variance each scenario actually produces.
 *
 * presence.c and position.c gate on var/mean^2 against fixed absolute
 * thresholds, so this is the quantity those thresholds must bracket. It is
 * duplicated here deliberately: the point is to observe the discriminant
 * independently of the code under test, so the thresholds can be checked
 * against measured values rather than assumed ones.
 */
static float scenario_norm_var(const synth_cfg_t *cfg, int frames)
{
    synth_state_t *sy = synth_create(cfg);
    csi_frame_t f;
    static float amp[2048];
    if (frames > 2048)
        frames = 2048;

    for (int i = 0; i < frames; i++) {
        synth_next(sy, &f);
        int nsub = f.data_num > 0 ? f.data_num : 64;
        double s = 0;
        for (int k = 0; k < nsub; k++)
            s += sqrt((double)f.data_i[k] * f.data_i[k] +
                      (double)f.data_q[k] * f.data_q[k]);
        amp[i] = (float)(s / nsub);
    }
    synth_destroy(sy);

    double mean = 0;
    for (int i = 0; i < frames; i++)
        mean += amp[i];
    mean /= frames;
    double var = 0;
    for (int i = 0; i < frames; i++) {
        double d = amp[i] - mean;
        var += d * d;
    }
    var /= frames;
    return (float)(var / (mean * mean + 1e-6));
}

static void calibrate(void)
{
    static const float reflects[] = { 0.05f, 0.10f, 0.20f, 0.40f, 0.60f };
    static const float snrs[]     = { 30.0f, 20.0f, 12.0f };

    printf("normalised amplitude variance (var/mean^2) by scenario\n");
    printf("presence.c gates: PRESENT > 0.15, ABSENT < 0.05\n");
    printf("position.c gates: zone assigned only when > 0.05\n\n");
    printf("%-10s %8s %12s %12s %12s\n",
           "reflect", "snr", "empty", "still", "moving");
    printf("----------------------------------------------------------\n");

    for (unsigned r = 0; r < sizeof(reflects) / sizeof(reflects[0]); r++) {
        for (unsigned s = 0; s < sizeof(snrs) / sizeof(snrs[0]); s++) {
            synth_cfg_t cfg;
            float v[3];
            scene_t order[3] = { SCENE_EMPTY, SCENE_STILL, SCENE_MOVING };
            for (int k = 0; k < 3; k++) {
                scene_cfg(&cfg, order[k], reflects[r], snrs[s], 0.0f, 1);
                v[k] = scenario_norm_var(&cfg, MOTION_FRAMES);
            }
            printf("%-10.2f %8.0f %12.2e %12.2e %12.2e\n",
                   (double)reflects[r], (double)snrs[s],
                   (double)v[0], (double)v[1], (double)v[2]);
        }
    }
}

int main(int argc, char **argv)
{
    int seeds = 12;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--calibrate") == 0) {
            calibrate();
            return 0;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seeds") == 0 && i + 1 < argc)
            seeds = atoi(argv[++i]);
    }
    if (seeds < 1)
        seeds = 1;

    static const float reflects[] = { 0.05f, 0.10f, 0.20f, 0.40f, 0.60f };
    static const float snrs[]     = { 30.0f, 20.0f, 12.0f };
    static const float drifts[]   = { 0.0f, 0.10f };
    const int n_ref = 5, n_snr = 3, n_drift = 2;

    printf("motion harness: %d seeds/config, %d frames/trial\n",
           seeds, MOTION_FRAMES);

    /* ---- presence ---- */
    printf("\npresence: detection vs reflector strength\n");
    printf("%-10s %12s %12s %12s\n",
           "reflect", "still_det%", "moving_det%", "empty_ok%");
    printf("------------------------------------------------------\n");

    int tot_still = 0, tot_still_ok = 0;
    int tot_move = 0, tot_move_ok = 0;
    int tot_empty = 0, tot_empty_ok = 0;

    for (int r = 0; r < n_ref; r++) {
        int still = 0, still_ok = 0, move = 0, move_ok = 0, empty = 0, empty_ok = 0;
        for (int si = 0; si < n_snr; si++) {
            for (int di = 0; di < n_drift; di++) {
                for (int s = 0; s < seeds; s++) {
                    synth_cfg_t cfg;
                    unsigned seed = (unsigned)(s + 1);

                    scene_cfg(&cfg, SCENE_STILL, reflects[r], snrs[si], drifts[di], seed);
                    still++; if (run_presence(&cfg)) still_ok++;

                    scene_cfg(&cfg, SCENE_MOVING, reflects[r], snrs[si], drifts[di], seed);
                    move++; if (run_presence(&cfg)) move_ok++;

                    scene_cfg(&cfg, SCENE_EMPTY, reflects[r], snrs[si], drifts[di], seed);
                    empty++; if (!run_presence(&cfg)) empty_ok++;
                }
            }
        }
        printf("%-10.2f %11.1f%% %11.1f%% %11.1f%%\n", (double)reflects[r],
               100.0 * still_ok / still, 100.0 * move_ok / move,
               100.0 * empty_ok / empty);

        tot_still += still; tot_still_ok += still_ok;
        tot_move  += move;  tot_move_ok  += move_ok;
        tot_empty += empty; tot_empty_ok += empty_ok;
    }

    double sens_still = 100.0 * tot_still_ok / tot_still;
    double sens_move  = 100.0 * tot_move_ok / tot_move;
    double spec       = 100.0 * tot_empty_ok / tot_empty;
    double balanced   = (0.5 * (sens_still + sens_move) + spec) / 2.0;

    printf("%-10s %11.1f%% %11.1f%% %11.1f%%\n", "ALL",
           sens_still, sens_move, spec);

    /* ---- position: empty room must report no zone ---- */
    int pos_empty = 0, pos_empty_ok = 0;
    for (int si = 0; si < n_snr; si++) {
        for (int di = 0; di < n_drift; di++) {
            for (int s = 0; s < seeds; s++) {
                synth_cfg_t cfg;
                scene_cfg(&cfg, SCENE_EMPTY, 0.0f, snrs[si], drifts[di],
                          (unsigned)(s + 1));
                pos_empty++;
                if (run_position(&cfg) < 0)
                    pos_empty_ok++;
            }
        }
    }
    double pos_ok = 100.0 * pos_empty_ok / pos_empty;
    printf("\nposition: empty room reports no zone in %.1f%% of %d trials\n",
           pos_ok, pos_empty);
    printf("  (zone accuracy is not scoreable with this signal model)\n");

    /* ---- gesture: shape conformance ---- */
    printf("\ngesture: label vs driven burst shape (spec conformance, not accuracy)\n");
    printf("%-12s %10s %10s\n", "shape", "expect", "match%");
    printf("----------------------------------------\n");

    struct { env_t e; const char *want; const char *name; } cases[] = {
        { ENV_CENTRE,  "wave", "centre-peak" },
        { ENV_RISING,  "push", "rising"      },
        { ENV_FALLING, "pull", "falling"     },
    };

    int g_tot = 0, g_ok = 0;
    for (unsigned ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        int n = 0, ok = 0;
        for (int r = 0; r < n_ref; r++) {
            for (int s = 0; s < seeds; s++) {
                const char *got = run_gesture(cases[ci].e, reflects[r],
                                              30.0f, (unsigned)(s + 1));
                n++;
                if (strcmp(got, cases[ci].want) == 0)
                    ok++;
            }
        }
        printf("%-12s %10s %9.1f%%\n", cases[ci].name, cases[ci].want,
               100.0 * ok / n);
        g_tot += n; g_ok += ok;
    }

    printf("\nSUMMARY presence_still=%.1f%% presence_moving=%.1f%% presence_empty=%.1f%%"
           " balanced=%.1f%% position_empty_ok=%.1f%% gesture_conform=%.1f%%\n",
           sens_still, sens_move, spec, balanced, pos_ok,
           100.0 * g_ok / g_tot);
    return 0;
}
