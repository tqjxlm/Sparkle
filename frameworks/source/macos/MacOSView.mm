#if FRAMEWORK_MACOS

#import "MacOSView.h"

#include "application/AppFramework.h"
#include "application/NativeKeyboard.h"
#include "core/CoreStates.h"

@implementation MacOSView

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [[self window] makeFirstResponder:self];

    [self initApp];
}

- (void)keyDown:(NSEvent *)theEvent
{
    NSString *text = [theEvent charactersIgnoringModifiers];
    // we want to separate the shift key from the actual key
    auto modifier_flags = theEvent.modifierFlags;
    bool shift_on = (modifier_flags & NSEventModifierFlagShift) != 0;
    text = [theEvent charactersByApplyingModifiers:(modifier_flags & ~NSEventModifierFlagShift)];

    unichar keychar = (text.length > 0) ? [text.lowercaseString characterAtIndex:0] : 0;
    // support keyboards with no escape key
    if (keychar == static_cast<unichar>(sparkle::NativeKeyboard::KEY_DELETE))
    {
        keychar = static_cast<unichar>(sparkle::NativeKeyboard::KEY_ESCAPE);
    }

    app_->KeyboardCallback(keychar, sparkle::AppFramework::KeyAction::Press, shift_on);
}

- (void)keyUp:(NSEvent *)theEvent
{
    NSString *text = [theEvent charactersIgnoringModifiers];

    // we want to separate the shift key from the actual key
    auto modifier_flags = theEvent.modifierFlags;
    bool shift_on = (modifier_flags & NSEventModifierFlagShift) != 0;
    text = [theEvent charactersByApplyingModifiers:(modifier_flags & ~NSEventModifierFlagShift)];

    unichar keychar = (text.length > 0) ? [text.lowercaseString characterAtIndex:0] : 0;
    app_->KeyboardCallback(keychar, sparkle::AppFramework::KeyAction::Release, shift_on);
}

- (NSPoint)getMouseLocalPoint:(NSEvent *)theEvent
{
    NSPoint location = [theEvent locationInWindow];
    NSPoint point = [self convertPointToBacking:location];
    point.y = self.frame.size.height * self.window.backingScaleFactor - point.y;
    return point;
}

- (void)mouseDown:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left, sparkle::AppFramework::KeyAction::Press,
                              0);
}

- (void)mouseUp:(NSEvent *)theEvent
{
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Primary_Left,
                              sparkle::AppFramework::KeyAction::Release, 0);
}

- (void)rightMouseDown:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Secondary_Right,
                              sparkle::AppFramework::KeyAction::Press, 0);
}

- (void)rightMouseUp:(NSEvent *)theEvent
{
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Secondary_Right,
                              sparkle::AppFramework::KeyAction::Release, 0);
}

- (void)otherMouseDown:(NSEvent *)theEvent
{
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Count, sparkle::AppFramework::KeyAction::Press, 0);
}

- (void)otherMouseUp:(NSEvent *)theEvent
{
    app_->MouseButtonCallback(sparkle::AppFramework::ClickButton::Count, sparkle::AppFramework::KeyAction::Release, 0);
}

- (void)mouseDragged:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
}

- (void)rightMouseDragged:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
}

- (void)otherMouseDragged:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
}

- (void)mouseMoved:(NSEvent *)theEvent
{
    auto point = [self getMouseLocalPoint:theEvent];
    app_->CursorPositionCallback(point.x, point.y);
}

- (void)scrollWheel:(NSEvent *)theEvent
{
    auto yoffset = [theEvent deltaY];
    auto xoffset = [theEvent deltaX];
    app_->ScrollCallback(xoffset, yoffset);
}

@end

#endif
