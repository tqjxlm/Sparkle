#ifndef REBLUR_DATA_H
#define REBLUR_DATA_H

#include "reblur_common.h.slang"

// Internal data packing: diffAccumSpeed (6 bits) + specAccumSpeed (6 bits)
// Stored in RG16Float as normalized [0,1] values
#define REBLUR_ACCUMSPEED_BITS    6
#define REBLUR_MAX_ACCUM_FRAME    ((1 << REBLUR_ACCUMSPEED_BITS) - 1)

float2 PackInternalData(float diff_accum_speed, float spec_accum_speed)
{
    return float2(diff_accum_speed / REBLUR_MAX_ACCUM_FRAME, spec_accum_speed / REBLUR_MAX_ACCUM_FRAME);
}

void UnpackInternalData(float2 packed, out float diff_accum_speed, out float spec_accum_speed)
{
    diff_accum_speed = packed.x * REBLUR_MAX_ACCUM_FRAME;
    spec_accum_speed = packed.y * REBLUR_MAX_ACCUM_FRAME;
}

// Non-linear accumulation speed: f = 1 / (1 + N)
float GetNonLinearAccumSpeed(float accum_speed)
{
    return 1.0 / (1.0 + accum_speed);
}

#endif
