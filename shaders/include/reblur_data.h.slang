#ifndef REBLUR_DATA_H
#define REBLUR_DATA_H

#include "reblur_common.h.slang"

// Internal data packing: store accumSpeed directly in RG16Float channels.
// Float16 represents integers exactly up to 2048, so no normalization needed.
float2 PackInternalData(float diff_accum_speed, float spec_accum_speed)
{
    return float2(diff_accum_speed, spec_accum_speed);
}

void UnpackInternalData(float2 packed, out float diff_accum_speed, out float spec_accum_speed)
{
    diff_accum_speed = packed.x;
    spec_accum_speed = packed.y;
}

// Non-linear accumulation speed: f = 1 / (1 + N)
float GetNonLinearAccumSpeed(float accum_speed)
{
    return 1.0 / (1.0 + accum_speed);
}

#endif
