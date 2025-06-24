#if FRAMEWORK_APPLE

#include "MetalRenderTarget.h"

#include "MetalContext.h"
#include "MetalImage.h"

namespace sparkle
{
void MetalRenderTarget::Init()
{
    if (IsBackBufferTarget())
    {
        SetColorImage(context->GetBackBufferColor(), 0);
    }
}
} // namespace sparkle

#endif
