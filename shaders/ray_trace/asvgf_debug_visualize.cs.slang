// S1 scaffolding shader: visualize ASVGF stage/view routing with deterministic color patterns.

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint stage;
    uint view;
    uint history_index;
    uint frame_index;
    uint resolution_x;
    uint resolution_y;
    uint freeze_history;
    uint force_clear_history;
};

[[vk::binding(1, 0)]] RWTexture2D<float4> outputImage;

float3 HashToColor(uint value)
{
    float3 color;
    color.r = ((value >> 0) & 255) / 255.0;
    color.g = ((value >> 8) & 255) / 255.0;
    color.b = ((value >> 16) & 255) / 255.0;
    return color;
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution_x || pixel.y >= resolution_y)
    {
        return;
    }

    uint hash_value = (stage + 1u) * 73856093u;
    hash_value ^= (view + 1u) * 19349663u;
    hash_value ^= (history_index + 1u) * 83492791u;

    float3 base = HashToColor(hash_value);
    float checker = (((pixel.x / 16u) + (pixel.y / 16u)) & 1u) == 0u ? 1.0 : 0.72;
    float pulse = (frame_index & 1u) == 0u ? 1.0 : 0.92;

    float3 color = saturate(base * checker * pulse);

    if (freeze_history != 0u)
    {
        color = saturate(color + float3(0.15, 0.0, 0.0));
    }

    if (force_clear_history != 0u)
    {
        color = saturate(color + float3(0.0, 0.15, 0.0));
    }

    outputImage[pixel] = float4(color, 1.0);
}
