#ifndef REBLUR_CONFIG_H
#define REBLUR_CONFIG_H

#define REBLUR_POISSON_SAMPLE_NUM             8
#define REBLUR_PRE_BLUR_FRACTION_SCALE        2.0
#define REBLUR_BLUR_FRACTION_SCALE            1.0
#define REBLUR_POST_BLUR_FRACTION_SCALE       0.5
#define REBLUR_POST_BLUR_RADIUS_SCALE         2.0
#define REBLUR_HISTORY_FIX_FILTER_RADIUS      2
#define REBLUR_ANTI_FIREFLY_FILTER_RADIUS     4
#define REBLUR_FAST_HISTORY_CLAMPING_RADIUS   2
#define REBLUR_DENOISING_RANGE                1000.0
#define REBLUR_TILE_SIZE                      16

// Temporal stabilization: minimum sigma as fraction of mean luminance.
// At convergence (spatial variance -> 0), sigma collapse forces history
// toward the local mean, which acts as spatial filtering to reduce frozen
// noise from the TA's EMA. This is the primary TS denoising mechanism
// (matching NVIDIA NRD behavior). Set to 0 for best noise reduction.
#define REBLUR_TS_MIN_SIGMA_FRACTION          0.0
#define REBLUR_TS_HISTORY_WARMUP_FRAMES       4.0
#define REBLUR_TS_MOTION_RESET_THRESHOLD_PX   1.0
#define REBLUR_TA_MOTION_RESET_THRESHOLD_PX   1.0
#define REBLUR_TA_MOTION_MAX_ACCUM            2.0
#define REBLUR_TA_BOUNDARY_MOTION_MAX_ACCUM   2.0
#define REBLUR_TA_CENTER_FALLBACK_MOTION_MAX_ACCUM 4.0
#define REBLUR_TA_SPEC_MOTION_CONFIDENCE_SCALE 0.5
#define REBLUR_TA_SPEC_MOTION_MIN_SENSITIVITY 0.5
#define REBLUR_TA_SPEC_MOTION_MIN_MAX_ACCUM   2.0
#define REBLUR_TA_SPEC_MOTION_MAX_ACCUM       3.0
#define REBLUR_TA_SPEC_VIRTUAL_MOTION_SCALE   16.0
#define REBLUR_TA_SPEC_MIN_VIRTUAL_LOBE_PX    0.02
#define REBLUR_TA_SPEC_MAX_VIRTUAL_LOBE_PX    1.0
#define REBLUR_TA_SPEC_HITDIST_CONFIDENCE_SCALE 8.0
#define REBLUR_TS_SPEC_HITDIST_BLEND_SCALE    2.0

// Anti-firefly constants
#define REBLUR_ANTI_FIREFLY_SIGMA_SCALE       2.0
#define REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY 38.0
#define REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE 0.3

#endif
