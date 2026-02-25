[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
};

[[vk::binding(1, 0)]] Texture2D<float4> denoisedDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> denoisedSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallic;
[[vk::binding(4, 0)]] RWTexture2D<float4> outputImage;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float4 diff = denoisedDiffuse[pixel];
    float4 spec = denoisedSpecular[pixel];
    float4 albedo = albedoMetallic[pixel];

    float3 color = diff.rgb * albedo.rgb + spec.rgb;

    outputImage[pixel] = float4(color, 1.0);
}
