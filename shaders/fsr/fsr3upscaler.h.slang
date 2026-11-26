// The options every FSR 3.1 upscaler pass compiles with: HDR color, motion vectors at render resolution without jitter,
// depth that grows away from the camera, 32-bit math, and a luma pyramid that synchronizes through groupshared memory
// instead of wave operations.
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HLSL_SM 60
#define FFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL 1
#define FFX_SPD_NO_WAVE_OPERATIONS 1
#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1
#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0
#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0

// each pass declares its entry point CS after its thread group size macros
#define FFX_FSR3UPSCALER_NUM_THREADS                                                                                   \
    [shader("compute")][numthreads(FFX_FSR3UPSCALER_THREAD_GROUP_WIDTH, FFX_FSR3UPSCALER_THREAD_GROUP_HEIGHT,          \
                                   FFX_FSR3UPSCALER_THREAD_GROUP_DEPTH)]
#define CS shader_main
