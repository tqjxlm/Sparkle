#pragma once

#if FRAMEWORK_IOS

#import "apple/MetalView.h"

@interface iOSView : MetalView <UIGestureRecognizerDelegate>

@end

#endif
