#ifndef COLOR_SLOT_H_
#define COLOR_SLOT_H_

// fragment outputs of the attachment roles at their ColorSlot (renderer/pass/ColorSlot.h). slang ignores the SV_Target
// index of a returned value on Metal and assigns SPIR-V output locations in declaration order, so each output is a
// struct member with an explicit location.
struct SceneColorOutput
{
    [[vk::location(1)]] float4 color : SV_Target1;
};

struct GBufferOutput
{
    [[vk::location(2)]] uint4 packed : SV_Target2;
    [[vk::location(3)]] float depth_copy : SV_Target3;
};

#endif // COLOR_SLOT_H_
