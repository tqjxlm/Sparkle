#pragma once

#if FRAMEWORK_APPLE

#include "rhi/MetalRHI.h"

#import <MetalKit/MetalKit.h>

#define DESCRIPTOR_SET_AS_ARGUMENT_BUFFER 0

namespace sparkle
{
template <class T> void SetDebugInfo(T &object, const std::string &label)
{
    if (label.length() > 0)
    {
        object.label = [NSString stringWithUTF8String:label.c_str()];
    }
}
} // namespace sparkle

#endif
