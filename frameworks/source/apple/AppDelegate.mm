#if FRAMEWORK_APPLE

#import "apple/AppDelegate.h"

#include "apple/AppleNativeView.h"
#include "application/AppFramework.h"

@interface AppDelegate ()

@end

@implementation AppDelegate
{
    sparkle::AppFramework *app_;
    sparkle::AppleNativeView *native_view_;
}

- (sparkle::AppFramework *)GetAppFramework
{
    return app_;
}

- (instancetype)init
{
    self = [super init];
    if (self)
    {
        NSArray<NSString *> *arguments = [[NSProcessInfo processInfo] arguments];

        const char *argv[1024];

        // Fill C-array with ints
        int argc = static_cast<int>([arguments count]);

        for (auto i = 0u; i < static_cast<unsigned>(argc); ++i)
        {
            argv[i] = [[arguments objectAtIndex:i] UTF8String];
        }

        app_ = new sparkle::AppFramework();

        native_view_ = new sparkle::AppleNativeView();
        app_->SetNativeView(native_view_);

        // early init of core functionalities (logger, threading, config)
        app_->InitCore(argc, argv);
    }
    return self;
}

- (void)applicationWillFinishLaunching:(NSNotification *)aNotification
{
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
    if (app_)
    {
        delete app_;
        delete native_view_;
        app_ = nullptr;
        native_view_ = nullptr;
    }
}

#if FRAMEWORK_MACOS
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return YES;
}
#elif FRAMEWORK_IOS
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    return YES;
}
#endif

@end

#endif
