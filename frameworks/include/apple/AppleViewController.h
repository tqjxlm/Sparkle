#pragma once

#if FRAMEWORK_APPLE

#import <MetalKit/MetalKit.h>

namespace sparkle
{
class AppFramework;
}

#if FRAMEWORK_MACOS

#import <AppKit/AppKit.h>

@interface AppleViewController : NSViewController <MTKViewDelegate>

#elif FRAMEWORK_IOS

#import <UiKit/UiKit.h>

@interface AppleViewController : UIViewController <MTKViewDelegate>

#endif

@end

#endif
