// sparkle replacement: the FidelityFX backend registry (ffx_provider.h) is not part of this subset. The FSR3 upscaler
// only needs this declaration, which Sparkle's backend defines.
#pragma once

#include "ffx_interface.h"

FfxErrorCode GetResourceSizeFromDescription(FfxDevice device, const FfxCreateResourceDescription *createResourceDescription,
                                            uint64_t *sizeInBytes, uint64_t *alignment = nullptr);
