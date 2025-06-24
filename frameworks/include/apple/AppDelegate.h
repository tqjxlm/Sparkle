#pragma once

#if FRAMEWORK_APPLE

namespace sparkle
{
class AppFramework;
} // namespace sparkle

#if FRAMEWORK_MACOS

#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>

- (sparkle::AppFramework *)GetAppFramework;

@end

#elif FRAMEWORK_IOS

#import <UIKit/UIKit.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate>

- (sparkle::AppFramework *)GetAppFramework;

@property(strong, nonatomic) UIWindow *window;

@end

#endif

#endif
