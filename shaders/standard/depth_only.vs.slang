[[vk::binding(0, 0)]] cbuffer view
{
    matrix ViewProjectionMatrix;
}

[[vk::binding(0, 1)]] cbuffer mesh
{
    matrix ModelMatrix;
    matrix ModelMatrixInvTranspose;
}

struct VS_INPUT
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VS_OUTPUT
{
    float4 gl_Position : SV_Position;
};

[shader("vertex")]
VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;

    float4 world_pos = mul(ModelMatrix, float4(input.position.xyz, 1.0f));

    float4 clip_pos = mul(ViewProjectionMatrix, world_pos);

    output.gl_Position = clip_pos;

    return output;
}