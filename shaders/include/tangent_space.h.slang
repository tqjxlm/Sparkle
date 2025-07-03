#ifndef TANGENT_SPACE_H_
#define TANGENT_SPACE_H_

#include "math.h.slang"

float AbsCosTheta(float3 w)
{
    return abs(w.z);
}

float CosTheta(float3 w)
{
    return w.z;
}

float3 Reflect(float3 w)
{
    return float3(-w.xy, w.z);
}

float3 Refract(float3 w_i, float eta_i_over_eta_t)
{
    float cos_theta_i = CosTheta(w_i);
    float sin_theta_i_2 = 1.f - cos_theta_i * cos_theta_i;
    float sin_theta_t_2 = eta_i_over_eta_t * eta_i_over_eta_t * sin_theta_i_2;

    float cos_theta_t = sqrt(1 - sin_theta_t_2) * (cos_theta_i > 0 ? -1.f : 1.f);

    return normalize(float3(-eta_i_over_eta_t * w_i.xy, cos_theta_t));
}

#endif // TANGENT_SPACE_H_