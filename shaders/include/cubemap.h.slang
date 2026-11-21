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

// the inverse of GetDirectionFromCubeMapUV
float2 GetCubeMapUVFromDirection(float3 direction, out uint face_id)
{
    float3 a = abs(direction);
    float2 uv;
    if (a.x >= a.y && a.x >= a.z)
    {
        face_id = direction.x > 0 ? 0 : 1;
        uv = float2(direction.x > 0 ? -direction.z : direction.z, -direction.y) / a.x;
    }
    else if (a.y >= a.z)
    {
        face_id = direction.y > 0 ? 2 : 3;
        uv = float2(direction.x, direction.y > 0 ? direction.z : -direction.z) / a.y;
    }
    else
    {
        face_id = direction.z > 0 ? 4 : 5;
        uv = float2(direction.z > 0 ? direction.x : -direction.x, -direction.y) / a.z;
    }

    return uv * 0.5f + 0.5f;
}

#endif // CUBEMAP_H_