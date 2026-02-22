#if FRAMEWORK_IOS

#import "iOSView.h"

#include "application/AppFramework.h"
#include "core/Exception.h"

#include <UIKit/UIKit.h>

@implementation iOSView

- (CGPoint)scaledLocationInAppCoordinates:(CGPoint)location
{
    const auto &render_config = app_->GetRenderConfig();
    const CGSize view_size = self.bounds.size;
    if (view_size.width <= 0.f || view_size.height <= 0.f)
    {
        return location;
    }

    const double scale_x = static_cast<double>(render_config.image_width) / static_cast<double>(view_size.width);
    const double scale_y = static_cast<double>(render_config.image_height) / static_cast<double>(view_size.height);
    return CGPointMake(location.x * scale_x, location.y * scale_y);
}

- (void)didMoveToWindow
{
    [super didMoveToWindow];

    [self initApp];

    {
        UIPinchGestureRecognizer *recognizer =
            [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
        recognizer.name = @"scene_pinch";
        recognizer.delegate = self;
        [self addGestureRecognizer:recognizer];
    }

    {
        UITapGestureRecognizer *recognizer = [[UITapGestureRecognizer alloc] initWithTarget:self
                                                                                     action:@selector(handleTap:)];
        recognizer.name = @"scene_tap";
        recognizer.delegate = self;
        [self addGestureRecognizer:recognizer];
    }

    {
        UITapGestureRecognizer *recognizer =
            [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleFourFingersTap:)];
        recognizer.name = @"scene_four_fingers_tap";
        recognizer.delegate = self;
        [recognizer setNumberOfTouchesRequired:4];
        [self addGestureRecognizer:recognizer];
    }

    {
        UIPanGestureRecognizer *recognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                                                     action:@selector(handlePan:)];
        recognizer.name = @"scene_pan";
        recognizer.delegate = self;
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
        auto location = [recognizer locationInView:self];
        auto scaled_location = [self scaledLocationInAppCoordinates:location];
        app_->CursorPositionCallback(scaled_location.x, scaled_location.y);
        app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                                  sparkle::AppFramework::KeyAction::Press, 0);
        app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                                  sparkle::AppFramework::KeyAction::Release, 0);
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
    auto location = [recognizer locationInView:self];
    auto scaled_location = [self scaledLocationInAppCoordinates:location];
    switch (recognizer.state)
    {
    case UIGestureRecognizerStateBegan:
        app_->CursorPositionCallback(scaled_location.x, scaled_location.y);
        app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                                  sparkle::AppFramework::KeyAction::Press, 0);
        break;
    case UIGestureRecognizerStateChanged:
        app_->CursorPositionCallback(scaled_location.x, scaled_location.y);
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
    (void)gestureRecognizer;
    (void)otherGestureRecognizer;
    return YES;
}

@end

#endif
