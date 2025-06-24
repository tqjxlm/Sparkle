#ifndef MESH_H_
#define MESH_H_

struct MeshPassVSInterpolant
{
    float3 tangent : TEXCOORD1;
    float3 bi_tangent : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float3 world_pos : TEXCOORD4;
    float2 uv : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

#endif // MESH_H_
