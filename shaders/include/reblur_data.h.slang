#ifndef REBLUR_DATA_H
#define REBLUR_DATA_H

#include "reblur_common.h.slang"

// Internal data packing: accumSpeed (6 bits) + accumSpeedFast (6 bits)
// Stored in RG16Float as normalized [0,1] values
#define REBLUR_ACCUMSPEED_BITS    6
#define REBLUR_MAX_ACCUM_FRAME    ((1 << REBLUR_ACCUMSPEED_BITS) - 1)

float2 PackInternalData(float accum_speed, float accum_speed_fast)
{
    return float2(accum_speed / REBLUR_MAX_ACCUM_FRAME, accum_speed_fast / REBLUR_MAX_ACCUM_FRAME);
}

void UnpackInternalData(float2 packed, out float accum_speed, out float accum_speed_fast)
{
    accum_speed = packed.x * REBLUR_MAX_ACCUM_FRAME;
    accum_speed_fast = packed.y * REBLUR_MAX_ACCUM_FRAME;
}

// Non-linear accumulation speed: f = 1 / (1 + N)
float GetNonLinearAccumSpeed(float accum_speed)
{
    return 1.0 / (1.0 + accum_speed);
}

#endif
