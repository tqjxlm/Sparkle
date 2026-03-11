[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
};

[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(4, 0)]] RWTexture2D<float4> outSpecular;

[shader("compute")][numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    outDiffuse[pixel] = inDiffuse[pixel];
    outSpecular[pixel] = inSpecular[pixel];
}
