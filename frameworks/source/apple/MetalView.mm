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

#include <mutex>
#include <queue>

@implementation MetalView
{
    dispatch_semaphore_t frames_in_flight_semaphore_;
    // pushed by the main thread, taken by the render thread
    std::mutex free_drawables_mutex_;
    std::queue<id<CAMetalDrawable>> free_drawables_;
    BOOL app_stopped_;
}

- (void)initApp
{
    if (app_stopped_ || app_)
    {
        return;
    }

#if FRAMEWORK_MACOS
    auto *delegate = (AppDelegate *)[NSApplication sharedApplication].delegate;
#elif FRAMEWORK_IOS
    auto *delegate = (AppDelegate *)[UIApplication sharedApplication].delegate;
#else
    UnImplemented();
#endif

    frames_in_flight_semaphore_ = dispatch_semaphore_create([self getMaxFrameInFlight]);

    app_ = [delegate GetAppFramework];
    ASSERT(app_);
    auto *native_view = static_cast<sparkle::AppleNativeView *>(app_->GetNativeView());

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
        sparkle::AppFramework::RequestExit();
        [self stopApp];
#endif
    }
}

#if FRAMEWORK_MACOS
// the scale must be in place before super resizes the drawable, which triggers the frame buffer resize
- (void)viewDidChangeBackingProperties
{
    if (app_ && !app_stopped_)
    {
        static_cast<sparkle::AppleNativeView *>(app_->GetNativeView())->UpdateWindowScale();
    }

    [super viewDidChangeBackingProperties];
}
#endif

- (void)stopApp
{
    if (app_stopped_)
    {
        return;
    }

    app_stopped_ = YES;
    self.paused = YES;
    self.delegate = nil;
#if FRAMEWORK_IOS
    self.userInteractionEnabled = NO;
#endif

    if (app_)
    {
        app_->Cleanup();
        app_ = nullptr;
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

    std::scoped_lock<std::mutex> lock(free_drawables_mutex_);
    free_drawables_.push(next_drawable);
}

- (id<CAMetalDrawable>)getNextDrawable
{
    std::scoped_lock<std::mutex> lock(free_drawables_mutex_);

    // the main thread acquires one drawable per main loop frame, but the render thread renders a frame for every batch
    // it runs, so a frame can find none, e.g. after the batch of a TaskManager::FlushRenderThread
    if (free_drawables_.empty())
    {
        return nil;
    }

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
