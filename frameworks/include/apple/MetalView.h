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

- (void)stopApp;

// main thread, once per main loop frame: acquires a drawable once a frame in flight completes
- (void)waitForNextDrawable;

// render thread: takes the oldest drawable the main thread acquired, or nil when every one has been taken
- (id<CAMetalDrawable>)getNextDrawable;

- (dispatch_semaphore_t)getInFlightSemaphore;

- (int)getMaxFrameInFlight;

@end

#endif
