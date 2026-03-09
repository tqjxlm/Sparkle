struct ConvergenceTileMetrics
{
    float sum_variance;
    float max_variance;
    uint active_pixels;
    uint padding;
};

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint previous_sample_count;
    uint current_batch_sample_count;
    uint tile_count_x;
};

[[vk::binding(1, 0)]] Texture2D<float2> currentSampleStats;
[[vk::binding(2, 0)]] Texture2D<float2> previousVarianceState;
[[vk::binding(3, 0)]] RWTexture2D<float2> nextVarianceState;
[[vk::binding(4, 0)]] RWStructuredBuffer<ConvergenceTileMetrics> outTileMetrics;

static const uint ThreadGroupSizeX = 16;
static const uint ThreadGroupSizeY = 16;
static const uint ThreadGroupSize = ThreadGroupSizeX * ThreadGroupSizeY;

groupshared float s_SumVariance[ThreadGroupSize];
groupshared float s_MaxVariance[ThreadGroupSize];
groupshared uint s_ActivePixels[ThreadGroupSize];

[shader("compute")][numthreads(ThreadGroupSizeX, ThreadGroupSizeY, 1)] void main(uint3 DTid : SV_DispatchThreadID,
                                                                                 uint3 GTid : SV_GroupThreadID,
                                                                                 uint3 Gid : SV_GroupID) {
    const uint local_index = GTid.y * ThreadGroupSizeX + GTid.x;

    float sum_variance = 0.0f;
    float max_variance = 0.0f;
    uint active_pixels = 0;

    if (DTid.x < resolution.x && DTid.y < resolution.y && current_batch_sample_count > 0)
    {
        const float2 current_stats = currentSampleStats.Load(int3(DTid.xy, 0));
        const float batch_mean = current_stats.x;
        const float batch_m2 = max(current_stats.y, 0.0f);
        const float batch_count = float(current_batch_sample_count);
        const float prev_count = float(previous_sample_count);
        const float total_count = prev_count + batch_count;

        float mean_next = batch_mean;
        float m2_next = batch_m2;

        if (previous_sample_count > 0)
        {
            const float2 previous_state = previousVarianceState.Load(int3(DTid.xy, 0));
            const float mean_prev = previous_state.x;
            const float m2_prev = max(previous_state.y, 0.0f);
            const float delta = batch_mean - mean_prev;
            mean_next = mean_prev + delta * (batch_count / total_count);
            m2_next = max(m2_prev + batch_m2 + delta * delta * (prev_count * batch_count / total_count), 0.0f);

            if (previous_sample_count > 1)
            {
                const float variance = (m2_next / max(total_count - 1.0f, 1.0f)) / total_count;

                sum_variance = variance;
                max_variance = variance;
                active_pixels = 1u;
            }
        }

        nextVarianceState[DTid.xy] = float2(mean_next, m2_next);
    }

    s_SumVariance[local_index] = sum_variance;
    s_MaxVariance[local_index] = max_variance;
    s_ActivePixels[local_index] = active_pixels;

    GroupMemoryBarrierWithGroupSync();

    for (uint stride = ThreadGroupSize / 2; stride > 0; stride >>= 1)
    {
        if (local_index < stride)
        {
            const uint other = local_index + stride;
            s_SumVariance[local_index] += s_SumVariance[other];
            s_MaxVariance[local_index] = max(s_MaxVariance[local_index], s_MaxVariance[other]);
            s_ActivePixels[local_index] += s_ActivePixels[other];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (local_index == 0)
    {
        const uint tile_index = Gid.y * tile_count_x + Gid.x;
        ConvergenceTileMetrics metrics;
        metrics.sum_variance = s_SumVariance[0];
        metrics.max_variance = s_MaxVariance[0];
        metrics.active_pixels = s_ActivePixels[0];
        metrics.padding = 0u;
        outTileMetrics[tile_index] = metrics;
    }
}
