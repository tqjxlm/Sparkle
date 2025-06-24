#if FRAMEWORK_APPLE

#if FRAMEWORK_MACOS
#import <Cocoa/Cocoa.h>
#elif FRAMEWORK_IOS
#import <UIKit/UIKit.h>
#endif

#import "apple/AppDelegate.h"

#if FRAMEWORK_MACOS

int main(int argc, const char *argv[])
{
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
