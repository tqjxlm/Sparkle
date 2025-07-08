#ifndef PBR_H_
#define PBR_H_

#include "directional_light.h.slang"
#include "sampler.h.slang"
#include "sky_light.h.slang"
#include "standard.h.slang"
#include "surface.h.slang"

const static int ibl_specular_mips = 4;

struct IBLConfig
{
    TextureCube diffuse;
    SamplerState diffuse_sampler;
    TextureCube specular;
    SamplerState specular_sampler;
    Texture2D brdf;
    SamplerState brdf_sampler;
};

float3 SampleDiffuseIBL(RenderConfig config, float3 dir, IBLConfig ibl)
{
    if (config.has_diffuse_ibl == 1)
    {
        return ibl.diffuse.Sample(ibl.diffuse_sampler, dir).rgb;
    }
    else
    {
        return (float3)0;
    }
}

float3 SampleSpecularIBL(RenderConfig config, float3 dir, float roughness, IBLConfig ibl)
{
    if (config.has_specular_ibl == 1)
    {
        return ibl.specular.SampleLevel(ibl.specular_sampler, dir, roughness * ibl_specular_mips).rgb;
    }
    else
    {
        return (float3)0;
    }
}

float3 CalculateDirectLighting(float3 V, float3 L, float3 radiance, SurfaceAttributes surface)
{
    float3 N = surface.normal;
    float3 H = normalize(V + L);
    float NdotL = SaturateDot(N, L);
    float NdotV = SaturateDot(N, V);
    float HdotV = SaturateDot(H, V);

    // micro surface
    float NDF = DistributionGGX::Evaluate(N, H, surface.roughness);
    float G = SmithGGX(NdotV, NdotL, surface.roughness);

    // F0: the base reflectance when light hits a surface straight on
    // this value is empirical and widely adopted.
    float3 F0 = (float3)0.04;
    F0 = lerp(F0, surface.albedo, surface.metallic);

    // fresnel
    float3 F = FresnelSchlick(HdotV, F0);

    // specular
    float3 numerator = NDF * G;
    float denominator = 4.0 * NdotV * NdotL + Eps;
    float3 specular = numerator / denominator;

    // for non-metal meterial, diffuse is what's left with specular
    float3 kS = F;
    float3 kD = (float3)1.0 - kS;

    // theoretically, surface.metallic can only be 0 or 1.
    // here we allow the possibility of values in between.
    kD *= 1.0 - surface.metallic;

    // diffuse
    float3 diffuse = surface.albedo * InvPi;

    return (kD * diffuse + kS * specular) * radiance * NdotL;
}

float3 CalculateAmbientLighting(RenderConfig config, float3 V, SkyLight sky_light, SurfaceAttributes surface,
                                IBLConfig ibl)
{
    float3 N = surface.normal;
    float NdotV = SaturateDot(N, V);
    float3 R = reflect(-V, N);

    float3 environment_lighting;

    if (config.has_specular_ibl == 1 || config.has_diffuse_ibl == 1)
    {
        // metallic workflow
        float3 F0 = (float3)0.04;
        F0 = lerp(F0, surface.albedo, surface.metallic);

        float3 F = FresnelSchlickRoughness(NdotV, F0, surface.roughness);
        float3 kS = F;

        float3 kD = 1.0 - kS;
        // theoretically, surface.metallic can only be 0 or 1.
        // here we allow the possibility of values in between.
        kD *= 1.0 - surface.metallic;

        // diffuse IBL
        float3 diffuse_ibl = SampleDiffuseIBL(config, N, ibl);
        float3 diffuse = diffuse_ibl * surface.albedo;

        // specular IBL
        float3 specular_ibl = SampleSpecularIBL(config, R, surface.roughness, ibl);
        float2 brdf = ibl.brdf.Sample(ibl.brdf_sampler, float2(NdotV, surface.roughness)).xy;
        float3 specular = specular_ibl * (F0 * brdf.x + brdf.y);

        environment_lighting = kD * diffuse + specular;
    }
    else
    {
        // a very rough approximation that treats the environment as a gradual light sphere

        float3 L = reflect(-V, N);

        float t = 0.5f * L.z + 0.5f;
        float3 ambient_sky_light = lerp((float3)1.0, sky_light.color, t);

        float NdotL = SaturateDot(N, L);
        environment_lighting = ambient_sky_light * surface.albedo * InvPi * NdotL;
    }

    return environment_lighting;
}

#endif // PBR_H_