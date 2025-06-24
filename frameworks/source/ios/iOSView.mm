#if FRAMEWORK_IOS

#import "iOSView.h"

#include "application/AppFramework.h"
#include "core/Exception.h"
#include "core/math/Types.h"

#include <UIKit/UIKit.h>

@implementation iOSView

- (void)didMoveToWindow
{
    [super didMoveToWindow];

    [self initApp];

    {
        UIPinchGestureRecognizer *recognizer =
            [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
        [self addGestureRecognizer:recognizer];
    }

    {
        UITapGestureRecognizer *recognizer = [[UITapGestureRecognizer alloc] initWithTarget:self
                                                                                     action:@selector(handleTap:)];
        [self addGestureRecognizer:recognizer];
    }

    {
        UITapGestureRecognizer *recognizer =
            [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleFourFingersTap:)];
        [recognizer setNumberOfTouchesRequired:4];
        [self addGestureRecognizer:recognizer];
    }

    {
        UIPanGestureRecognizer *recognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                                                     action:@selector(handlePan:)];
        [recognizer setMaximumNumberOfTouches:1];
        [self addGestureRecognizer:recognizer];
    }
}

- (void)handlePinch:(UIPinchGestureRecognizer *)recognizer
{
    const float sensitivity = 10.f;

    if (recognizer.state == UIGestureRecognizerStateBegan || recognizer.state == UIGestureRecognizerStateChanged)
    {
        CGFloat offset = (recognizer.scale - 1) * sensitivity;
        app_->ScrollCallback(-offset, -offset);

        // Reset the scale factor to 1 to allow incremental scaling
        recognizer.scale = 1.0;
    }
}

- (void)handleTap:(UITapGestureRecognizer *)recognizer
{
    if (recognizer.state == UIGestureRecognizerStateRecognized)
    {
        app_->ClickCallback();
    }
}

- (void)handleFourFingersTap:(UITapGestureRecognizer *)recognizer
{
    if (recognizer.state == UIGestureRecognizerStateRecognized)
    {
        app_->CaptureNextFrames(1);
    }
}

- (void)handlePan:(UIPanGestureRecognizer *)recognizer
{
    const float sensitivity = 3.f;

    auto location = [recognizer locationInView:self];
    sparkle::Vector2Int scaled_location{location.x * sensitivity, location.y * sensitivity};
    switch (recognizer.state)
    {
    case UIGestureRecognizerStateBegan:
        app_->CursorPositionCallback(scaled_location.x(), scaled_location.y());
        app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                                  sparkle::AppFramework::KeyAction::Press, 0);
        break;
    case UIGestureRecognizerStateChanged:
        app_->CursorPositionCallback(scaled_location.x(), scaled_location.y());
        break;
    case UIGestureRecognizerStateEnded:
        app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                                  sparkle::AppFramework::KeyAction::Release, 0);
        break;
    default:
        break;
    }
}

// Delegate method to allow simultaneous gestures
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer
{
    // Always return YES for simultaneous gestures
    return YES;
}

@end

#endif
