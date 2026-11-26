// AMD's luma pyramid pass binds all six SPD mips, but the pass touches only the last one. Slang keeps the other five,
// which exceeds Metal's limit of 8 read_write textures per kernel, so this entry declares mip 5 alone.
#include "fsr3upscaler.h.slang"

#define FSR3UPSCALER_BIND_SRV_CURRENT_LUMA 0
#define FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH 1

#define FSR3UPSCALER_BIND_UAV_SPD_GLOBAL_ATOMIC 0
#define FSR3UPSCALER_BIND_UAV_FRAME_INFO 1
#define FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH_MIP1 2

#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0
#define FSR3UPSCALER_BIND_CB_SPD 1

#include "fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h"
#include "fsr3upscaler/ffx_fsr3upscaler_common.h"

globallycoherent RWTexture2D<FfxFloat32x2> rw_spd_mip5;

FfxFloat32x2 RWLoadPyramid(FfxInt32x2 iPxPos, FfxUInt32 index)
{
    return rw_spd_mip5[iPxPos];
}

void StorePyramid(FfxInt32x2 iPxPos, FfxFloat32x2 outValue, FfxUInt32 index)
{
    rw_spd_mip5[iPxPos] = outValue;
}

#include "fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h"

#define FFX_FSR3UPSCALER_THREAD_GROUP_WIDTH 256
#define FFX_FSR3UPSCALER_THREAD_GROUP_HEIGHT 1
#define FFX_FSR3UPSCALER_THREAD_GROUP_DEPTH 1

FFX_FSR3UPSCALER_NUM_THREADS
void CS(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    ComputeAutoExposure(WorkGroupId, LocalThreadIndex);
}
