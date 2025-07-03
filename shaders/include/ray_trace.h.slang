#ifndef RAY_TRACE_H_
#define RAY_TRACE_H_

#include "debug.h.slang"
#include "random.h.slang"
#include "sampler.h.slang"
#include "tangent_space.h.slang"

struct Ray
{
    float3 origin;
    float3 direction;
};

struct MaterialParameters
{
    float3 baseColor;
    int baseColorTextureId;
    float3 emissiveColor;
    int emissiveTextureId;
    float metallic;
    float roughness;
    int metallicRoughnessTextureId;
    int normalTextureId;
    float eta;
};

struct VertexAttribute
{
    float4 tangent;
    float3 normal;
    float2 texCoord;
};

struct Intersection
{
    float4x3 inv_transform;
    float4 tangent;
    float3 position;
    uint materialId;
    float3 normal;
    float2 texCoord;
    uint primitiveId;
};

struct SampleResult
{
    bool is_valid;
    float3 throughput;
    float3 local_w_i;
};

SampleResult BxdfSpecular(float3 local_w_o, MaterialParameters mat_param)
{
    SampleResult result;
    result.is_valid = false;
    float3 local_w_m = SampleMicroFacetNormal(local_w_o, mat_param.roughness);
    float3 local_w_i = reflect(-local_w_o, local_w_m);

    float cos_o = CosTheta(local_w_o);
    float cos_i = CosTheta(local_w_i);
    float cos_m = CosTheta(local_w_m);
    float o_dot_m = SaturateDot(local_w_o, local_w_m);

    float3 fresnel_color;
    if (cos_o > Eps && cos_i > Eps && cos_m > Eps)
    {
        if (RandomUnit() < mat_param.metallic)
        {
            // Metal: all specular, Fresnel lerp between light color and albedo
            result.is_valid = true;
            fresnel_color = FresnelSchlick(o_dot_m, mat_param.baseColor);
        }
        else
        {
            // Non-metal: Fresnel tells how much is specular
            const float r0 = 0.04f;
            float fresnel = FresnelSchlick(o_dot_m, r0);

            fresnel_color = float3(1.0, 1.0, 1.0);

            if (RandomUnit() < fresnel)
            {
                result.is_valid = true;
            }
        }
    }

    if (result.is_valid)
    {
        float occlusion = SmithGGX(cos_o, cos_i, mat_param.roughness);
        float normalizer = GeometrySchlickGGX(o_dot_m, mat_param.roughness) + Eps;
        result.throughput = fresnel_color * occlusion / normalizer;
        result.local_w_i = local_w_i;
    }

    return result;
}

SampleResult BxdfLambertian(float3 local_w_o, MaterialParameters mat_param)
{
    SampleResult result;
    result.is_valid = true;
    result.local_w_i = SampleCosineWeightedHemisphere();
    result.throughput = mat_param.baseColor; // Lambertian diffuse reflectance

    return result;
}

float3 BxdfMetal(float3 w_o, out float3 w_i, MaterialParameters mat_param, Intersection intersection)
{
    // Convert world space direction to local (tangent) space
    float3 local_w_o = normalize(TransformBasisToLocal(w_o, intersection.normal, intersection.tangent.xyz));

    SampleResult result = BxdfSpecular(local_w_o, mat_param);

    if (!result.is_valid)
    {
        result = BxdfLambertian(local_w_o, mat_param);
    }

    // Convert the local direction back to world space
    w_i = normalize(TransformBasisToWorld(result.local_w_i, intersection.normal, intersection.tangent.xyz));

    return result.throughput;
}

float3 BxdfDieletric(float3 w_o, out float3 w_i, MaterialParameters mat_param, Intersection intersection)
{
    // Convert world space direction to local (tangent) space
    float3 local_w_o = normalize(TransformBasisToLocal(w_o, intersection.normal, intersection.tangent.xyz));
    float3 local_w_i;

    // Perfect dielectric materials
    const float eta_i_ = 1.0f;
    const float eta_t_ = mat_param.eta;

    float fresnel = FresnelDielectric(CosTheta(local_w_o), eta_i_, eta_t_);
    float behaviour_probability = RandomUnit();

    if (behaviour_probability < fresnel)
    {
        local_w_i = Reflect(local_w_o);
    }
    else
    {
        bool is_entering = CosTheta(local_w_o) > 0;
        float eta_i_over_eta_t = is_entering ? eta_i_ / eta_t_ : eta_t_ / eta_i_;
        local_w_i = Refract(local_w_o, eta_i_over_eta_t);
    }

    // Convert the local direction back to world space
    w_i = normalize(TransformBasisToWorld(local_w_i, intersection.normal, intersection.tangent.xyz));

    return mat_param.baseColor;
}

#endif // RAY_TRACE_H_