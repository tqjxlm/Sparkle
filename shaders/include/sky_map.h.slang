#ifndef SKY_MAP_H_
#define SKY_MAP_H_

struct VSInterpolant
{
    float3 local_pos : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

#endif // SKY_MAP_H_