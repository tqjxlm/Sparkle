#include "screen.h.slang"

[[vk::binding(0, 1)]] cbuffer ubo
{
    float exposure;
}

// PIXEL_LOCAL reads the scene color an earlier draw of the rendering wrote at this pixel (ColorSlot::SceneColor)
#ifdef PIXEL_LOCAL
[[vk::binding(1, 1)]] [[vk::input_attachment_index(1)]] SubpassInput screenTexture;
#else
[[vk::binding(1, 1)]] Texture2D screenTexture;
[[vk::binding(2, 1)]] SamplerState screenTextureSampler;
#endif

float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    x *= exposure;
    return clamp((x * (x * a + b)) / (x * (x * c + d) + e), 0.0f, 1.0f);
}

[shader("fragment")] float4 shader_main(VSInterpolant input) : SV_Target
{
#ifdef PIXEL_LOCAL
    float3 color = screenTexture.SubpassLoad().xyz;
#else
    float3 color = screenTexture.Sample(screenTextureSampler, input.uv).xyz;
#endif

    color = ACESFilm(color);

    return float4(color, 1.0f);
}