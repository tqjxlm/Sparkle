// Split the path tracer's provider-neutral packed inputs into the texture layout MetalFX expects.
// The color input is the progressive accumulator, not the per-frame noisy radiance: MetalFX averages
// in a non-linear space, so high-variance HDR input biases its output darker; the accumulator's
// noise shrinks as samples build up, and with it the bias.

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4x4 projection;
    uint2 resolution;
    float farDepth;
    float padding;
}

[[vk::binding(1, 0)]] Texture2D<float4> sceneRadiance;
[[vk::binding(2, 0)]] Texture2D<float4> normalViewDepth;
[[vk::binding(3, 0)]] Texture2D<float4> albedoObjectId;
[[vk::binding(4, 0)]] Texture2D<float4> motionHitMetallic;
[[vk::binding(5, 0)]] Texture2D<float4> specularAlbedoRoughness;

[[vk::binding(6, 0)]] RWTexture2D<float4> outColor;
[[vk::binding(7, 0)]] RWTexture2D<float> outDepth;
[[vk::binding(8, 0)]] RWTexture2D<float2> outMotion;
[[vk::binding(9, 0)]] RWTexture2D<float4> outDiffuseAlbedo;
[[vk::binding(10, 0)]] RWTexture2D<float4> outSpecularAlbedo;
[[vk::binding(11, 0)]] RWTexture2D<float4> outNormal;
[[vk::binding(12, 0)]] RWTexture2D<float> outRoughness;

[shader("compute")][numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float3 radiance = max(sceneRadiance.Load(int3(pixel, 0)).rgb, 0.f);
    float4 normal_depth = normalViewDepth.Load(int3(pixel, 0));
    float4 albedo = albedoObjectId.Load(int3(pixel, 0));
    float4 motion_hit_metallic = motionHitMetallic.Load(int3(pixel, 0));
    float4 specular_albedo_roughness = specularAlbedoRoughness.Load(int3(pixel, 0));
    bool is_hit = motion_hit_metallic.z > 0.5f;
    bool is_emissive = motion_hit_metallic.z > 1.5f;

    float depth = farDepth;
    if (is_hit)
    {
        float4 clip = mul(projection, float4(0.f, 0.f, -normal_depth.w, 1.f));
        depth = saturate(clip.z / clip.w);
    }

    float3 normal = is_hit ? normalize(normal_depth.xyz) : float3(0.f, 0.f, 1.f);
    float metallic = saturate(motion_hit_metallic.w);

    // sky and primary-emissive pixels are deterministic: albedo = radiance makes the demodulated
    // signal flat 1, so denoising cannot damage it and remodulation restores the exact detail
    bool demodulation_bypass = !is_hit || is_emissive;
    float3 diffuse_albedo = demodulation_bypass ? radiance : max(albedo.rgb, 0.f) * (1.f - metallic);
    float3 specular_albedo = demodulation_bypass ? float3(0.f, 0.f, 0.f) : max(specular_albedo_roughness.rgb, 0.f);
    float roughness = demodulation_bypass ? 1.f : saturate(specular_albedo_roughness.w);

    outColor[pixel] = float4(radiance, 1.f);
    outDepth[pixel] = depth;
    outMotion[pixel] = motion_hit_metallic.xy;
    outDiffuseAlbedo[pixel] = float4(diffuse_albedo, 1.f);
    outSpecularAlbedo[pixel] = float4(specular_albedo, 1.f);
    outNormal[pixel] = float4(normal, 1.f);
    outRoughness[pixel] = roughness;
}
