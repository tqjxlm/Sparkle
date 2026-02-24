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

#if FRAMEWORK_MACOS

namespace
{
bool IsTrueValue(const char *value)
{
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "True") == 0 ||
           strcmp(value, "TRUE") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "Yes") == 0 ||
           strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 || strcmp(value, "On") == 0 ||
           strcmp(value, "ON") == 0;
}

bool ShouldRunHeadless(int argc, const char *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--headless") != 0)
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
} // namespace

int main(int argc, const char *argv[])
{
    if (ShouldRunHeadless(argc, argv))
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
            return 0;
        }
    }

    @autoreleasepool
    {
        return NSApplicationMain(argc, argv);
    }
}

#elif FRAMEWORK_IOS

int main(int argc, char *argv[])
{

    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}

#endif

#endif
