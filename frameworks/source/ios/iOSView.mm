#if FRAMEWORK_IOS

#import "iOSView.h"

#include "application/AppFramework.h"
#include "application/InputEvents.h"

#include <UIKit/UIKit.h>

// stable small ids per active UITouch, as required by PointerEvent
static constexpr int MaxTouchSlots = 8;

@implementation iOSView
{
    UITouch *__weak touch_slots_[MaxTouchSlots];
}

// ui space on ios is render-target pixels, matching io.DisplaySize
- (sparkle::Vector2)uiPointFromTouch:(UITouch *)touch
{
    const CGPoint location = [touch locationInView:self];
    const CGSize view_size = self.bounds.size;
    if (view_size.width <= 0.0 || view_size.height <= 0.0)
    {
        return {static_cast<float>(location.x), static_cast<float>(location.y)};
    }

    const auto &render_config = app_->GetRenderConfig();
    const double scale_x = static_cast<double>(render_config.image_width) / static_cast<double>(view_size.width);
    const double scale_y = static_cast<double>(render_config.image_height) / static_cast<double>(view_size.height);
    return {static_cast<float>(location.x * scale_x), static_cast<float>(location.y * scale_y)};
}

- (void)didMoveToWindow
{
    [super didMoveToWindow];

    self.multipleTouchEnabled = YES;

    [self initApp];
}

- (int)slotForTouch:(UITouch *)touch assignIfMissing:(BOOL)assign
{
    for (int i = 0; i < MaxTouchSlots; i++)
    {
        if (touch_slots_[i] == touch)
        {
            return i;
        }
    }

    if (!assign)
    {
        return -1;
    }

    for (int i = 0; i < MaxTouchSlots; i++)
    {
        if (!touch_slots_[i])
        {
            touch_slots_[i] = touch;
            return i;
        }
    }

    return -1;
}

- (void)pushTouches:(NSSet<UITouch *> *)touches action:(sparkle::PointerAction)action
{
    if (!app_)
    {
        return;
    }

    for (UITouch *touch in touches)
    {
        const BOOL is_begin = action == sparkle::PointerAction::Down;
        const int slot = [self slotForTouch:touch assignIfMissing:is_begin];
        if (slot < 0)
        {
            continue;
        }

        if (action == sparkle::PointerAction::Up || action == sparkle::PointerAction::Cancel)
        {
            touch_slots_[slot] = nil;
        }

        app_->PushInputEvent(sparkle::PointerEvent{.device = sparkle::PointerDevice::Touch,
                                                   .action = action,
                                                   .id = static_cast<uint8_t>(slot),
                                                   .position = [self uiPointFromTouch:touch]});
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self pushTouches:touches action:sparkle::PointerAction::Down];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self pushTouches:touches action:sparkle::PointerAction::Move];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self pushTouches:touches action:sparkle::PointerAction::Up];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self pushTouches:touches action:sparkle::PointerAction::Cancel];
}

@end

#endif
