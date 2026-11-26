// Clears one mip of a float or normalized image to a color.

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4 value;
}

[[vk::binding(1, 0)]] RWTexture2D<float4> target;

[shader("compute")][numthreads(8, 8, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 size;
    target.GetDimensions(size.x, size.y);
    if (all(DTid.xy < size))
    {
        target[DTid.xy] = value;
    }
}
