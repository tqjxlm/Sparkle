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

// Anti-firefly constants
#define REBLUR_ANTI_FIREFLY_SIGMA_SCALE       2.0
#define REBLUR_FIREFLY_SUPPRESSOR_MAX_RELATIVE_INTENSITY 38.0
#define REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE 0.3

#endif
