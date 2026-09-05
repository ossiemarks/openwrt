#include <math.h>
#include <string.h>
#include <stdio.h>
#include "presence.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Presence from the normalised amplitude variance, measured against a learned
 * quiet-channel floor rather than a fixed absolute threshold.
 *
 * An absolute threshold cannot work here. The empty-room variance is dominated
 * by receiver noise, so the floor moves by more than an order of magnitude
 * across the usable SNR range, while a weak reflector at high SNR can sit
 * below a strong reflector's floor at low SNR. Any single constant is therefore
 * correct at one operating point and wrong everywhere else. Comparing against
 * the floor the receiver is actually seeing removes that dependency.
 *
 * The floor is learned only from samples that read as confidently quiet, so an
 * occupant cannot be absorbed into the baseline. This assumes the room is
 * unoccupied often enough for the floor to be learned; a room occupied
 * continuously from the very first frame will read as empty.
 *
 * The ratios below set the operating point on a shallow trade-off curve. They
 * are calibrated against a synthetic channel model, NOT against real CSI, so
 * they should be re-derived once real capture is available. Measured against
 * that model, holding everything else fixed:
 *
 *   ratio 3.0/1.8   still 47.5%  moving 61.7%  empty 86.1%
 *   ratio 2.5/1.5   still 56.1%  moving 70.3%  empty 81.9%   <- chosen
 *   ratio 2.0/1.4   still 60.8%  moving 74.7%  empty 79.2%
 *   ratio 1.8/1.3   still 64.7%  moving 76.7%  empty 77.8%
 *
 * Lower ratios score marginally better on balanced accuracy but do so purely
 * by trading empty-room specificity for sensitivity, and the best value sat at
 * the edge of the swept range. A mid-range point is taken instead of the
 * argmax, since false presence in an empty room is the more costly error here.
 */
#define WINDOW_SIZE       100
#define PRESENT_RATIO     2.5f
#define ABSENT_RATIO      1.5f
#define FLOOR_ADAPT       0.01f
#define FLOOR_DOWN        0.05f
#define FLOOR_WARMUP      100
#define FLOOR_MIN         1e-9f

static float window[WINDOW_SIZE];
static int   wpos, wfull;
static bool  last_published;
static float noise_floor;
static int   floor_warmup;
static bool  believe_present;

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
    noise_floor = 0.0f;
    floor_warmup = 0;
    believe_present = false;
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

    /*
     * Learn the quiet floor before making any call. The floor estimates the
     * mean quiet variance, not the minimum: tracking the minimum biases the
     * floor below the typical quiet level, which inflates every ratio and
     * produces false positives in an empty room.
     */
    if (floor_warmup < FLOOR_WARMUP) {
        noise_floor += norm_var;
        if (++floor_warmup == FLOOR_WARMUP)
            noise_floor /= FLOOR_WARMUP;
        return;
    }

    float ratio = norm_var / noise_floor;

    bool present;
    float confidence;
    if (ratio > PRESENT_RATIO) {
        present = true;
        confidence = fminf(1.0f, (ratio - PRESENT_RATIO) / (PRESENT_RATIO * 2.0f));
    } else if (ratio < ABSENT_RATIO) {
        present = false;
        confidence = fminf(1.0f, (ABSENT_RATIO - ratio) / ABSENT_RATIO);
    } else {
        return; /* hysteresis zone: hold last state */
    }
    believe_present = present;

    /*
     * Learn the floor only from samples that read as confidently quiet.
     *
     * Adapting whenever presence is merely not yet asserted is not sufficient:
     * the detector starts out believing the room is empty, so the floor climbs
     * toward the occupied variance and absorbs the occupant before the ratio
     * ever crosses the threshold. The fast downward path stays available in
     * all states so a floor learned too high can still recover.
     */
    float adapt = 0.0f;
    if (!present)
        adapt = FLOOR_ADAPT;
    else if (norm_var < noise_floor)
        adapt = FLOOR_DOWN;
    noise_floor += (norm_var - noise_floor) * adapt;
    if (noise_floor < FLOOR_MIN)
        noise_floor = FLOOR_MIN;

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
