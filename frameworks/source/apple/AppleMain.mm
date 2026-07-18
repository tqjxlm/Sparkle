#if FRAMEWORK_APPLE

#if FRAMEWORK_MACOS
#import <Cocoa/Cocoa.h>
#elif FRAMEWORK_IOS
#import <UIKit/UIKit.h>
#endif

#include "apple/AppleNativeView.h"
#include "application/AppFramework.h"
#include "core/CoreStates.h"

#import "apple/AppDelegate.h"

#include <cstring>

namespace
{
bool IsTrueValue(const char *value)
{
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "True") == 0 ||
           strcmp(value, "TRUE") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "Yes") == 0 ||
           strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 || strcmp(value, "On") == 0 ||
           strcmp(value, "ON") == 0;
}

bool HasFlag(int argc, const char *const argv[], const char *flag)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], flag) != 0)
        {
            continue;
        }

        if (i + 1 >= argc)
        {
            return true;
        }

        const char *next = argv[i + 1];
        if (strncmp(next, "--", 2) == 0)
        {
            return true;
        }

        return IsTrueValue(next);
    }

    return false;
}

// the windowless entry shared by macos and ios: no AppKit/UIKit is ever touched,
// so an ios build can run it as a plain process inside the simulator
int RunHeadless(int argc, const char *const argv[])
{
    @autoreleasepool
    {
        sparkle::AppleNativeView view;

        sparkle::AppFramework app;
        app.InitCore(argc, argv);
        app.SetNativeView(&view);

        if (!app.Init())
        {
            return 1;
        }

        while (!sparkle::CoreStates::IsExiting() && !view.ShouldClose())
        {
            app.MainLoop();
        }

        app.Cleanup();
#if ENABLE_TEST_CASES
        return app.GetExitCode();
#else
        return 0;
#endif
    }
}
} // namespace

#if FRAMEWORK_MACOS

int main(int argc, const char *argv[])
{
    // both must be decided before NSApplicationMain: neither path may touch AppKit
    if (HasFlag(argc, argv, "--cook"))
    {
        @autoreleasepool
        {
            sparkle::AppleNativeView view;

            sparkle::AppFramework app;
            app.InitCore(argc, argv);
            app.SetNativeView(&view);
            return app.RunCookMode();
        }
    }

    if (HasFlag(argc, argv, "--headless"))
    {
        return RunHeadless(argc, argv);
    }

    @autoreleasepool
    {
        return NSApplicationMain(argc, argv);
    }
}

#elif FRAMEWORK_IOS

int main(int argc, char *argv[])
{
    // must be decided before UIApplicationMain: the headless path may not touch UIKit
    if (HasFlag(argc, argv, "--headless"))
    {
        return RunHeadless(argc, argv);
    }

    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}

#endif

#endif
