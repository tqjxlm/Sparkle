#include "gbuffer.h.slang"
#include "mesh.h.slang"
#include "sampler.h.slang"
#include "standard.h.slang"
#include "surface.h.slang"

[[vk::binding(0, 1)]] cbuffer mesh
{
    matrix ModelMatrix;
    matrix ModelMatrixInvTranspose;
}

[[vk::binding(0, 2)]] cbuffer material
{
    MaterialParameters mat_param;
}

[[vk::binding(1, 2)]] Texture2D base_color_texture;
[[vk::binding(2, 2)]] Texture2D normal_texture;
[[vk::binding(3, 2)]] Texture2D metallic_roughness_texture;
[[vk::binding(4, 2)]] Texture2D emissive_texture;
[[vk::binding(5, 2)]] SamplerState material_texture_sampler;

float3 SampleNormal(float2 tex_coord)
{
    return normal_texture.Sample(material_texture_sampler, tex_coord).xyz * 2.0 - 1.0;
}

SurfaceAttributes LoadSurface(MeshPassVSInterpolant vs_in)
{
    SurfaceAttributes surface;

    if (mat_param.normal_texture >= 0)
    {
        float3 tangent_normal_sampled = SampleNormal(vs_in.uv);
        surface.normal =
            normalize(TransformBasisToWorld(tangent_normal_sampled, vs_in.tangent, vs_in.bi_tangent, vs_in.normal));
    }
    else
    {
        surface.normal = vs_in.normal;
    }

    surface.normal = normalize(mul((float3x3)ModelMatrixInvTranspose, surface.normal));

    float4 metallic_roughness_sampled = mat_param.metallic_roughness_texture >= 0
                                            ? metallic_roughness_texture.Sample(material_texture_sampler, vs_in.uv)
                                            : (float4)1;

    surface.roughness = metallic_roughness_sampled.y * mat_param.roughness;
    surface.metallic = metallic_roughness_sampled.z * mat_param.metallic;

    float3 base_color_sampled = mat_param.base_color_texture >= 0
                                    ? base_color_texture.Sample(material_texture_sampler, vs_in.uv).rgb
                                    : (float3)1;
    surface.albedo = mat_param.base_color * base_color_sampled;

    float3 emissive_sampled =
        mat_param.emissive_texture >= 0 ? emissive_texture.Sample(material_texture_sampler, vs_in.uv).rgb : (float3)1;
    surface.emissive = mat_param.emissive_color * emissive_sampled;

    return surface;
}

[shader("fragment")]
void main(MeshPassVSInterpolant vs_in, out uint4 gbuffer : SV_Target0)
{
    SurfaceAttributes surface = LoadSurface(vs_in);

    gbuffer = EncodeSurfaceToGBuffer(surface);
}