#if FRAMEWORK_APPLE

#include "apple/MetalView.h"

#import "apple/AppDelegate.h"
#include "apple/AppleNativeView.h"
#include "apple/AppleViewController.h"
#include "application/AppFramework.h"
#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/Profiler.h"

@implementation MetalView
{
    dispatch_semaphore_t frames_in_flight_semaphore_;
    std::queue<id<CAMetalDrawable>> free_drawables_;
}

- (void)initApp
{
#if FRAMEWORK_MACOS
    auto *delegate = (AppDelegate *)[NSApplication sharedApplication].delegate;
#elif FRAMEWORK_IOS
    auto *delegate = (AppDelegate *)[UIApplication sharedApplication].delegate;
#else
    UnImplemented();
#endif

    frames_in_flight_semaphore_ = dispatch_semaphore_create([self getMaxFrameInFlight]);

    app_ = [delegate GetAppFramework];
    auto *native_view = static_cast<sparkle::AppleNativeView *>(app_->GetNativeView());

    ASSERT(app_);

    [self setColorPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];

    Log(Info, "Initial bound size [{}, {}]", self.bounds.size.width, self.bounds.size.height);

#if FRAMEWORK_MACOS
    const sparkle::RenderConfig &render_config = app_->GetRenderConfig();
    [self.window setContentSize:{static_cast<CGFloat>(render_config.image_width),
                                 static_cast<CGFloat>(render_config.image_height)}];
    [self.window center];
#endif

    Log(Info, "Actual bound size [{}, {}]", self.bounds.size.width, self.bounds.size.height);
    Log(Info, "Actual drawable size [{}, {}]", self.drawableSize.width, self.drawableSize.height);

    native_view->SetMetalView(self);

    // now we have a native view for presenting, fully init the app
    if (!app_->Init())
    {
#if FRAMEWORK_MACOS
        [NSApp terminate:nil];
#else
        exit(0);
#endif
    }
}

- (void)waitForNextDrawable
{
    PROFILE_SCOPE("Metal waiting for drawable");

    // wait for a free drawable and consume one slot from the semaphore
    // the free slot will be released when the drawable is presented, see MetalContext::EndFrame
    dispatch_semaphore_wait(frames_in_flight_semaphore_, DISPATCH_TIME_FOREVER);

    auto next_drawable = [self currentDrawable];

    // with frames_in_flight_semaphore_, we should never get a nil drawable
    ASSERT(next_drawable);

    free_drawables_.push(next_drawable);
}

- (id<CAMetalDrawable>)getNextDrawable
{
    // with frames_in_flight_semaphore_, we should always have a drawable
    ASSERT(!free_drawables_.empty());

    auto next_drawable = free_drawables_.front();
    free_drawables_.pop();

    return next_drawable;
}

- (dispatch_semaphore_t)getInFlightSemaphore
{
    return frames_in_flight_semaphore_;
}

- (int)getMaxFrameInFlight
{
    return 3;
}

@end

#endif
