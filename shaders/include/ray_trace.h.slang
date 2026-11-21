#ifndef RAY_TRACE_H_
#define RAY_TRACE_H_

#include "debug.h.slang"
#include "pbr.h.slang"
#include "random.h.slang"
#include "sampler.h.slang"
#include "standard.h.slang"
#include "tangent_space.h.slang"

struct Ray
{
    float3 origin;
    float3 direction;
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
    uint triangleId;
};

// one slot of Vose's alias table, as built by BuildAliasTable in libraries/include/core/math/AliasTable.h
struct AliasTableEntry
{
    float threshold;
    uint alias;
    float probability;
};

// picks an entry with its probability in O(1)
uint SampleAliasTable(StructuredBuffer<AliasTableEntry> table, uint count, out float probability)
{
    uint index = min(uint(RandomUnit() * count), count - 1);
    if (RandomUnit() >= table[index].threshold)
    {
        index = table[index].alias;
    }

    probability = table[index].probability;
    return index;
}

// GGX roughness below this overflows the distribution's peak in fp32; it still renders as a mirror
static const float MinGgxRoughness = 0.08f;

// the metallic-roughness BSDF: a GGX specular lobe with Schlick fresnel, and a lambertian lobe for the light that the
// fresnel transmits into a dielectric. it is the BSDF CalculateDirectLightingSplit evaluates for raster lighting.
struct PbrBsdfValue
{
    // f * cos_i of each lobe
    float3 specular;
    float3 diffuse;
    // the pdf of SamplePbrBsdf generating w_i
    float pdf;
};

float SpecularLobeProbability(MaterialParameters mat_param, float3 f0, float cos_o)
{
    float specular = Luminance(FresnelSchlick(cos_o, f0));
    float diffuse = (1 - mat_param.metallic) * Luminance(mat_param.base_color) * (1 - specular);
    return specular / (specular + diffuse + Eps);
}

PbrBsdfValue EvaluatePbrBsdf(float3 normal, float3 w_o, float3 w_i, MaterialParameters mat_param)
{
    PbrBsdfValue value = (PbrBsdfValue)0;

    float cos_o = dot(normal, w_o);
    float cos_i = dot(normal, w_i);
    if (cos_o <= Eps || cos_i <= Eps)
    {
        return value;
    }

    float roughness = max(mat_param.roughness, MinGgxRoughness);
    float3 w_m = normalize(w_o + w_i);
    float3 f0 = ComputeF0(mat_param.base_color, mat_param.metallic);
    float3 fresnel = FresnelSchlick(dot(w_o, w_m), f0);
    float ndf = DistributionGGX::Ndf(dot(normal, w_m), roughness);

    value.specular = fresnel * ndf * SmithGGX(cos_o, cos_i, roughness) / (4 * cos_o);
    value.diffuse = (1 - fresnel) * (1 - mat_param.metallic) * mat_param.base_color * InvPi * cos_i;

    // the VNDF pdf of w_m times the jacobian 1 / (4 * dot(w_o, w_m)) of the reflection
    float specular_pdf = GeometrySchlickGGX(cos_o, roughness) * ndf / (4 * cos_o);
    value.pdf = lerp(cos_i * InvPi, specular_pdf, SpecularLobeProbability(mat_param, f0, cos_o));

    return value;
}

// picks one lobe to sample w_i from, and weights it by both lobes over their mixed pdf, so the estimate is the one
// EvaluatePbrBsdf describes. returns f * cos_i / pdf
float3 SamplePbrBsdf(float3 w_o, out float3 w_i, out float out_pdf, out bool is_specular_lobe,
                     MaterialParameters mat_param, Intersection intersection)
{
    float3 axis_u, axis_v, axis_w;
    GetLocalAxisFromNormal(intersection.normal, intersection.tangent.xyz, axis_u, axis_v, axis_w);
    float3 local_w_o = normalize(TransformBasisToLocal(w_o, axis_u, axis_v, axis_w));

    float3 f0 = ComputeF0(mat_param.base_color, mat_param.metallic);
    is_specular_lobe = RandomUnit() < SpecularLobeProbability(mat_param, f0, dot(intersection.normal, w_o));

    float3 local_w_i;
    if (is_specular_lobe)
    {
        float roughness = max(mat_param.roughness, MinGgxRoughness);
        local_w_i = reflect(-local_w_o, SampleMicroFacetNormal(local_w_o, roughness));
    }
    else
    {
        local_w_i = CosineWeightedHemisphere::Sample();
    }

    w_i = normalize(TransformBasisToWorld(local_w_i, axis_u, axis_v, axis_w));

    PbrBsdfValue value = EvaluatePbrBsdf(intersection.normal, w_o, w_i, mat_param);
    out_pdf = value.pdf;

    return value.pdf > 0 ? (value.specular + value.diffuse) / value.pdf : (float3)0;
}

float3 BxdfDieletric(float3 w_o, out float3 w_i, out float out_pdf, out bool is_specular_lobe,
                     MaterialParameters mat_param, Intersection intersection)
{
    // dielectric reflection and refraction are both sharp view-dependent paths -> specular channel.
    is_specular_lobe = true;

    // enter tangent space
    float3 axis_u, axis_v, axis_w;
    GetLocalAxisFromNormal(intersection.normal, intersection.tangent.xyz, axis_u, axis_v, axis_w);
    float3 local_w_o = normalize(TransformBasisToLocal(w_o, axis_u, axis_v, axis_w));
    float3 local_w_i;

    // Perfect dielectric materials
    const float eta_i_ = 1.0f;
    const float eta_t_ = mat_param.eta;

    float fresnel = FresnelDielectric(CosTheta(local_w_o), eta_i_, eta_t_);
    float behaviour_probability = RandomUnit();

    if (behaviour_probability < fresnel)
    {
        // Reflection
        local_w_i = Reflect(local_w_o);
        out_pdf = fresnel; // Probability of reflection
    }
    else
    {
        // Refraction
        bool is_entering = CosTheta(local_w_o) > 0;
        float eta_i_over_eta_t = is_entering ? eta_i_ / eta_t_ : eta_t_ / eta_i_;
        local_w_i = Refract(local_w_o, eta_i_over_eta_t);
        out_pdf = 1.0f - fresnel; // Probability of refraction
    }

    // back to world space
    w_i = normalize(TransformBasisToWorld(local_w_i, axis_u, axis_v, axis_w));

    return mat_param.base_color;
}

#endif // RAY_TRACE_H_