#pragma once

#if FRAMEWORK_APPLE

namespace sparkle
{
class AppFramework;
class AppleNativeView;
} // namespace sparkle

#import <MetalKit/MTKView.h>

@interface MetalView : MTKView
{
@protected
    sparkle::AppFramework *app_;
}

- (void)initApp;

- (void)waitForNextDrawable;

- (id<CAMetalDrawable>)getNextDrawable;

- (dispatch_semaphore_t)getInFlightSemaphore;

- (int)getMaxFrameInFlight;

@end

#endif
