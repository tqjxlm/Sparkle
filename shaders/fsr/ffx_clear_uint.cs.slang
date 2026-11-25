// Clears one mip of an unsigned integer image to the bits of a float, as a clear of its color value would.

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4 value;
}

[[vk::binding(1, 0)]] RWTexture2D<uint> target;

[shader("compute")][numthreads(8, 8, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 size;
    target.GetDimensions(size.x, size.y);
    if (all(DTid.xy < size))
    {
        target[DTid.xy] = asuint(value.x);
    }
}
