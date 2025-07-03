#ifndef CUBEMAP_H_
#define CUBEMAP_H_

#include "math.h.slang"

float3 GetDirectionFromCubeMapUV(float2 uv, uint face_id)
{
    float u = uv.x * 2.f - 1.f;
    float v = uv.y * 2.f - 1.f;

    float3 direction;
    switch (face_id)
    {
    case 0:
        direction = float3(1.f, -v, -u);
        break;
    case 1:
        direction = float3(-1.f, -v, u);
        break;
    case 2:
        direction = float3(u, 1.f, v);
        break;
    case 3:
        direction = float3(u, -1.f, -v);
        break;
    case 4:
        direction = float3(u, -v, 1.f);
        break;
    case 5:
        direction = float3(-u, -v, -1.f);
        break;
    default:
        // should not happen
        return float3(0, 0, 0);
    }

    return normalize(direction);
}

#endif // CUBEMAP_H_