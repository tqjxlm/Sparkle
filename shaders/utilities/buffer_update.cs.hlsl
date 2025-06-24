[[vk::binding(0, 0)]] StructuredBuffer<uint> indexBuffer : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> dataBuffer : register(t1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> outputBuffer : register(u0);

[[vk::binding(3, 0)]] cbuffer ubo : register(b0)
{
    uint element_size;
    uint element_count;
};

[numthreads(64, 1, 1)] void main(uint3 DTid
                                 : SV_DispatchThreadID) {
    uint thread_index = DTid.x;
    if (thread_index >= element_count)
    {
        return;
    }

    uint data_index = indexBuffer[thread_index];

    uint src_offset = thread_index * element_size;
    uint dst_offset = data_index * element_size;

    for (uint i = 0; i < element_size; i++)
    {
        outputBuffer[dst_offset + i] = dataBuffer[src_offset + i];
    }
}
