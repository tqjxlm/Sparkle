#if FRAMEWORK_MACOS

#import "MacOSView.h"

#include "application/AppFramework.h"
#include "application/InputEvents.h"

@implementation MacOSView
{
    NSTrackingArea *tracking_area_;
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [[self window] makeFirstResponder:self];

    [self initApp];
}

// mouseMoved: is only delivered inside a tracking area. imgui hover depends on it.
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];

    if (tracking_area_)
    {
        [self removeTrackingArea:tracking_area_];
    }

    tracking_area_ = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingInVisibleRect | NSTrackingActiveInKeyWindow)
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking_area_];
}

static uint32_t GetKeyboardModifiers(NSEvent *event)
{
    uint32_t modifiers = 0;
    if (event.modifierFlags & NSEventModifierFlagControl)
    {
        modifiers |= static_cast<uint32_t>(sparkle::KeyboardModifier::Control);
    }
    if (event.modifierFlags & NSEventModifierFlagShift)
    {
        modifiers |= static_cast<uint32_t>(sparkle::KeyboardModifier::Shift);
    }
    return modifiers;
}

// ui space: view-local points with the origin at the top left
- (sparkle::Vector2)uiPointFromEvent:(NSEvent *)theEvent
{
    NSPoint point = [self convertPoint:theEvent.locationInWindow fromView:nil];
    return {static_cast<float>(point.x), static_cast<float>(self.bounds.size.height - point.y)};
}

- (void)pushKeyEvent:(NSEvent *)theEvent action:(sparkle::KeyAction)action
{
    // separate the shift key from the actual key
    NSString *text = [theEvent charactersByApplyingModifiers:(theEvent.modifierFlags & ~NSEventModifierFlagShift)];

    unichar keychar = (text.length > 0) ? [text.lowercaseString characterAtIndex:0] : 0;

    app_->PushInputEvent(
        sparkle::KeyEvent{.key = keychar, .action = action, .modifiers = GetKeyboardModifiers(theEvent)});
}

- (void)keyDown:(NSEvent *)theEvent
{
    [self pushKeyEvent:theEvent action:sparkle::KeyAction::Press];

    // printable characters feed ui text input. surrogate pairs must be recombined:
    // NSString iterates utf-16 units, but CharEvent carries whole codepoints.
    if (!(theEvent.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
    {
        NSString *characters = theEvent.characters;
        for (NSUInteger i = 0; i < characters.length; i++)
        {
            uint32_t codepoint = [characters characterAtIndex:i];
            if (CFStringIsSurrogateHighCharacter(static_cast<UniChar>(codepoint)) && i + 1 < characters.length)
            {
                const unichar low = [characters characterAtIndex:i + 1];
                if (CFStringIsSurrogateLowCharacter(low))
                {
                    codepoint = CFStringGetLongCharacterForSurrogatePair(static_cast<UniChar>(codepoint), low);
                    i++;
                }
            }
            if (codepoint >= 0x20 && codepoint != 0x7F && (codepoint < 0xF700 || codepoint > 0xF7FF))
            {
                app_->PushInputEvent(sparkle::CharEvent{.codepoint = codepoint});
            }
        }
    }
}

- (void)keyUp:(NSEvent *)theEvent
{
    [self pushKeyEvent:theEvent action:sparkle::KeyAction::Release];
}

- (void)pushPointerEvent:(NSEvent *)theEvent action:(sparkle::PointerAction)action button:(sparkle::ClickButton)button
{
    app_->PushInputEvent(sparkle::PointerEvent{.action = action,
                                               .button = button,
                                               .modifiers = GetKeyboardModifiers(theEvent),
                                               .position = [self uiPointFromEvent:theEvent]});
}

- (void)mouseDown:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Down button:sparkle::ClickButton::PrimaryLeft];
}

- (void)mouseUp:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Up button:sparkle::ClickButton::PrimaryLeft];
}

- (void)rightMouseDown:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Down button:sparkle::ClickButton::SecondaryRight];
}

- (void)rightMouseUp:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Up button:sparkle::ClickButton::SecondaryRight];
}

- (void)mouseDragged:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Move button:sparkle::ClickButton::PrimaryLeft];
}

- (void)rightMouseDragged:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Move button:sparkle::ClickButton::SecondaryRight];
}

- (void)mouseMoved:(NSEvent *)theEvent
{
    [self pushPointerEvent:theEvent action:sparkle::PointerAction::Move button:sparkle::ClickButton::PrimaryLeft];
}

- (void)scrollWheel:(NSEvent *)theEvent
{
    app_->PushInputEvent(
        sparkle::ScrollEvent{.delta = {static_cast<float>(theEvent.deltaX), static_cast<float>(theEvent.deltaY)}});
}

@end

#endif
