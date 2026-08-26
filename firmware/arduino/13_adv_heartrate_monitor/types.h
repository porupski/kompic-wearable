/*
 * types.h  --  local type definitions for 13_adv_heartrate_monitor.ino
 *
 * These live in a header so Arduino IDE's auto-prototype pass (which inserts
 * function prototypes ABOVE the first typedef in the .ino) can see the types
 * when it generates prototypes for chan_init / chan_process / etc. Without
 * this file the auto-generated prototypes reference chan_t before it's
 * declared and every function using it fails to compile.
 */

#pragma once
#include <stdint.h>

// Per-channel signal-chain state (structurally identical for MAX + LSM)
typedef struct {
    // Baseline EMA (DC estimate for AC extraction)
    float    baseline;
    bool     baseline_ready;
    uint32_t baseline_start_ms;

    // 2nd-order Butterworth biquad state (Direct-Form I on AC signal)
    float    bp_x1, bp_x2, bp_y1, bp_y2;
    float    bp_b0, bp_b1, bp_b2, bp_a1, bp_a2;
    float    bp_out;

    // Envelope (leaky-max) of |bp|
    float    env;

    // AC signal previous value + slope tracking for motion gate
    float    ac_prev;
    float    slope_env;      // running expectation of |dAC|
    uint32_t motion_until_ms;

    // Peak detector state
    float    peak_prev;
    uint32_t last_beat_ms;
    uint32_t beat_count;
    uint32_t beat_count_valid;   // beats that survived motion gate

    // IBI history for regularity (CV)
    float    ibi_hist[8];        // QUAL_HISTORY -- kept literal here so
                                 // types.h doesn't depend on the .ino
    uint8_t  ibi_hist_n;
    uint8_t  ibi_hist_i;

    // Rolling autocorrelation buffer (bp signal)
    float   *ac_ring;
    int      ac_ring_n;
    int      ac_ring_cap;
    int      ac_ring_head;
    uint32_t last_autocorr_ms;
    float    autocorr_bpm;
    float    autocorr_peak_ratio;   // strength [0..1]

    // Live BPM (peak-detect derived, EMA of instantaneous 60000/IBI)
    float    bpm_pk;

    // Composite quality (0..100)
    float    quality;

    // Per-step aggregate (reset at each step boundary in main loop)
    uint32_t step_samples;
    float    step_dc_sum;
    float    step_ac_absmax;
    uint32_t step_motion_samples;
    uint32_t step_beats;
} chan_t;

typedef struct {
    uint8_t  led_pa;
    uint32_t t_start_ms;
    uint32_t t_end_ms;
    // MAX
    float    max_pi;
    float    max_bpm_pk;
    float    max_bpm_ac;
    float    max_motion_pct;
    float    max_quality;
    float    max_dc_mean;
    uint32_t max_beats;
    // LSM
    float    lsm_bpm_pk;
    float    lsm_bpm_ac;
    float    lsm_motion_pct;
    float    lsm_quality;
    uint32_t lsm_beats;
} step_summary_t;
