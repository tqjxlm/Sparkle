[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float2 params;
};

[[vk::binding(1, 0)]] Texture2D<float> in_view_z;
[[vk::binding(2, 0)]] RWTexture2D<uint> out_tiles;

groupshared uint shared_invalid_count;
groupshared uint shared_sample_count;

bool IsDenoisable(float view_z)
{
    if (isnan(view_z) || isinf(view_z))
    {
        return false;
    }

    float denoising_range = params.x;
    return view_z > 0.0f && view_z <= denoising_range;
}

[shader("compute")][numthreads(8, 4, 1)] void main(uint3 group_thread_id : SV_GroupThreadID,
                                                   uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex) {
    if (group_index == 0)
    {
        shared_invalid_count = 0u;
        shared_sample_count = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    uint2 tile_origin = group_id.xy * 16u;
    uint2 sample_base = tile_origin + group_thread_id.xy * uint2(2u, 4u);

    uint local_invalid_count = 0u;
    uint local_sample_count = 0u;

    [unroll] for (uint sample_x = 0; sample_x < 2u; sample_x++)
    {
        [unroll] for (uint sample_y = 0; sample_y < 4u; sample_y++)
        {
            uint2 pixel = sample_base + uint2(sample_x, sample_y);
            if (pixel.x >= resolution.x || pixel.y >= resolution.y)
            {
                continue;
            }

            local_sample_count += 1u;
            float view_z = in_view_z.Load(int3(pixel, 0));
            if (!IsDenoisable(view_z))
            {
                local_invalid_count += 1u;
            }
        }
    }

    InterlockedAdd(shared_invalid_count, local_invalid_count);
    InterlockedAdd(shared_sample_count, local_sample_count);

    GroupMemoryBarrierWithGroupSync();

    if (group_index == 0)
    {
        uint is_sky_tile = (shared_sample_count > 0u && shared_invalid_count == shared_sample_count) ? 1u : 0u;
        out_tiles[group_id.xy] = is_sky_tile;
    }
}
