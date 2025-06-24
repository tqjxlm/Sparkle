#if FRAMEWORK_APPLE

#include "apple/AppleViewController.h"

#import "apple/AppDelegate.h"
#include "apple/MetalView.h"
#include "application/AppFramework.h"
#include "core/CoreStates.h"
#include "core/Exception.h"

@implementation AppleViewController
{
    MetalView *_view;
    sparkle::AppFramework *app_;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

#if FRAMEWORK_MACOS
    auto *delegate = (AppDelegate *)[NSApplication sharedApplication].delegate;
#elif FRAMEWORK_IOS
    auto *delegate = (AppDelegate *)[UIApplication sharedApplication].delegate;
#else
    UnImplemented();
#endif
    app_ = [delegate GetAppFramework];

    ASSERT(app_);

    _view = (MetalView *)self.view;
#if FRAMEWORK_MACOS
    _view.wantsLayer = YES;
    _view.layer.backgroundColor = [NSColor colorWithRed:0 green:0 blue:0 alpha:1.0].CGColor;
#endif
#if FRAMEWORK_IOS
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
#endif
    _view.device = MTLCreateSystemDefaultDevice();
    [_view setPreferredFramesPerSecond:240];

    ASSERT_F(_view.device, "Metal is not supported on this device");

    Log(Info, "AppleViewController loaded");

    _view.delegate = self;
}

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size
{
    if (!app_)
    {
        return;
    }

    app_->FrameBufferResizeCallback(size.width, size.height);
}

- (void)drawInMTKView:(nonnull MTKView *)view
{
    if (!app_)
    {
        return;
    }

    if (sparkle::CoreStates::IsExiting())
    {
#if FRAMEWORK_MACOS
        [[NSApplication sharedApplication] terminate:nil];
#else
        // TODO(tqjxlm): exit gracefully for other platforms
        exit(0);
#endif
    }
    else
    {
        // metal requires that drawables should be acquired in the main thread
        [(MetalView *)view waitForNextDrawable];

        app_->MainLoop();
    }
}

@end

#endif
