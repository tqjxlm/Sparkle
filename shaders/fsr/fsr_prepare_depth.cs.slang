// Converts the path tracer's primary-hit view distance to the device depth FSR reconstructs geometry from. Sky pixels
// sit on the far plane.

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4x4 projection;
    uint2 resolution;
}

[[vk::binding(1, 0)]] Texture2D<float4> normalViewDepth;
[[vk::binding(2, 0)]] Texture2D<float4> motionHitMetallic;
[[vk::binding(3, 0)]] RWTexture2D<float> outDepth;

[shader("compute")][numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float depth = 1.f;
    if (motionHitMetallic.Load(int3(pixel, 0)).z > 0.5f)
    {
        float4 clip = mul(projection, float4(0.f, 0.f, -normalViewDepth.Load(int3(pixel, 0)).w, 1.f));
        depth = saturate(clip.z / clip.w);
    }
    outDepth[pixel] = depth;
}
