#ifndef SKY_LIGHT_H_
#define SKY_LIGHT_H_

// field order is layout-critical: a float3 must not be followed by a scalar (std140/MSL divergence)
struct SkyLight
{
    int has_sky_map;
    float3 color;
};

#endif // SKY_LIGHT_H_