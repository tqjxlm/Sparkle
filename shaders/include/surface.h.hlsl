#ifndef SURFACE_H_
#define SURFACE_H_

struct SurfaceAttributes
{
    float3 normal;
    float metallic;
    float3 albedo;
    float roughness;
    float3 emissive;
};

#endif // SURFACE_H_
