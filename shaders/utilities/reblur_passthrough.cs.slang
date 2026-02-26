[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
};

[[vk::binding(1, 0)]] Texture2D<float4> noisy_input;
[[vk::binding(2, 0)]] RWTexture2D<float4> denoised_output;

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;

    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    denoised_output[pixel] = noisy_input.Load(int3(pixel, 0));
}
