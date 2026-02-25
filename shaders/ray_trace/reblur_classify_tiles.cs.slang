#include "reblur_config.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float denoising_range;
};

[[vk::binding(1, 0)]] Texture2D<float> inViewZ;
[[vk::binding(2, 0)]] RWTexture2D<float> outTiles;

groupshared uint s_isSky;

[shader("compute")]
[numthreads(REBLUR_TILE_SIZE, REBLUR_TILE_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    if (GTid.x == 0 && GTid.y == 0)
        s_isSky = 1;

    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = Gid.xy * REBLUR_TILE_SIZE + GTid.xy;

    if (pixel.x < resolution.x && pixel.y < resolution.y)
    {
        float viewZ = inViewZ[pixel];
        if (viewZ <= denoising_range)
            InterlockedAnd(s_isSky, 0);
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x == 0 && GTid.y == 0)
        outTiles[Gid.xy] = float(s_isSky);
}
