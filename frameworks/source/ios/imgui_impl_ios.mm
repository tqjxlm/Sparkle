#import "imgui_impl_ios.h"
#include <imgui.h>

#if FRAMEWORK_IOS

#ifndef IMGUI_DISABLE

#include "core/math/Utilities.h"
#import "iOSView.h"

#import <CoreFoundation/CoreFoundation.h>
#import <MetalKit/MTKView.h>
#include <UIKit/UIGestureRecognizer.h>
#include <UIKit/UIKit.h>

#include <ctime>

static CFTimeInterval GetMachAbsoluteTimeInSeconds()
{
    return (CFTimeInterval)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
}

static sparkle::Vector2Int LocalViewToUiViewCoordinates(ImGuiIO &io, iOSView *view, const CGPoint &point)
{
    const float scale = io.DisplaySize.y / view.bounds.size.height;
    return {point.x * scale, point.y * scale};
}

struct ImGuiMouseEvent
{
    sparkle::Vector2Int position;
    bool down;
};

@interface ImGuiEventHandler : NSObject

- (instancetype)initWithView:(iOSView *)view NS_DESIGNATED_INITIALIZER;

- (instancetype)init NS_UNAVAILABLE;

@end

@implementation ImGuiEventHandler
{
    iOSView *view_;
    UITapGestureRecognizer *tap_recognizer_;
    UIPanGestureRecognizer *pan_recognizer_;
}

- (void)RegisterEventHandlers
{
    tap_recognizer_ = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    tap_recognizer_.delegate = view_;
    [view_ addGestureRecognizer:tap_recognizer_];

    pan_recognizer_ = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    pan_recognizer_.delegate = view_;
    [pan_recognizer_ setMaximumNumberOfTouches:1];
    [view_ addGestureRecognizer:pan_recognizer_];
}

- (void)handleTap:(UITapGestureRecognizer *)recognizer
{
    if (recognizer.state != UIGestureRecognizerStateRecognized)
    {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    auto location = [recognizer locationInView:view_];
    auto scaled_location = LocalViewToUiViewCoordinates(io, view_, location);

    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(scaled_location.x(), scaled_location.y());
    io.AddMouseButtonEvent(0, true);
    io.AddMouseButtonEvent(0, false);

    // reset mouse position to avoid hovering
    io.AddMousePosEvent(-1, -1);
}

- (void)handlePan:(UIPanGestureRecognizer *)recognizer
{
    if (recognizer.state == UIGestureRecognizerStateChanged || recognizer.state == UIGestureRecognizerStateBegan ||
        recognizer.state == UIGestureRecognizerStateRecognized)
    {
        ImGuiIO &io = ImGui::GetIO();

        auto translation = [recognizer translationInView:view_];
        auto scaled_translation = LocalViewToUiViewCoordinates(io, view_, translation);

        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMouseWheelEvent(scaled_translation.x(), scaled_translation.y());
    }
}

- (instancetype)initWithView:(iOSView *)view
{
    self = [super init];
    if (self)
    {
        view_ = view;
        [self RegisterEventHandlers];
    }
    return self;
}

- (void)dealloc
{
    if (view_)
    {
        if (tap_recognizer_)
        {
            [view_ removeGestureRecognizer:tap_recognizer_];
        }
        if (pan_recognizer_)
        {
            [view_ removeGestureRecognizer:pan_recognizer_];
        }
    }
}

@end

// NOLINTBEGIN(google-objc-function-naming,readability-identifier-naming)

struct ImGui_ImplIOS_Data
{
    double time_ = 0.0;
    ImGuiEventHandler *event_handler_;

    ImGui_ImplIOS_Data()
    {
        memset((void *)this, 0, sizeof(*this));
    }
};

static ImGui_ImplIOS_Data *ImGui_ImplIOS_CreateBackendData()
{
    return IM_NEW(ImGui_ImplIOS_Data)();
}

static ImGui_ImplIOS_Data *ImGui_ImplIOS_GetBackendData()
{
    return (ImGui_ImplIOS_Data *)ImGui::GetIO().BackendPlatformUserData;
}

static void ImGui_ImplIOS_DestroyBackendData()
{
    IM_DELETE(ImGui_ImplIOS_GetBackendData());
}

bool ImGui_ImplIOS_Init(MTKView *view)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplIOS_Data *bd = ImGui_ImplIOS_CreateBackendData();
    io.BackendPlatformUserData = (void *)bd;

    io.BackendPlatformName = "imgui_impl_ios";

    bd->event_handler_ = [[ImGuiEventHandler alloc] initWithView:(iOSView *)view];

    return true;
}

void ImGui_ImplIOS_Shutdown()
{
    ImGui_ImplIOS_Data *bd = ImGui_ImplIOS_GetBackendData();
    ASSERT_F(bd != nullptr, "No platform backend to shutdown, or already shutdown?");

    ImGui_ImplIOS_DestroyBackendData();

    ImGuiIO &io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasGamepad);
}

void ImGui_ImplIOS_NewFrame(MTKView *view)
{
    ImGui_ImplIOS_Data *bd = ImGui_ImplIOS_GetBackendData();
    ImGuiIO &io = ImGui::GetIO();

    // Setup display size
    if (view)
    {
        // const float dpi = (float)[view.window contentScaleFactor];
        // io.DisplayFramebufferScale = ImVec2(dpi, dpi);
    }

    // Setup time step
    if (bd->time_ == 0.0)
        bd->time_ = GetMachAbsoluteTimeInSeconds();

    auto current_time = GetMachAbsoluteTimeInSeconds();
    io.DeltaTime = static_cast<float>(current_time - bd->time_);
    bd->time_ = current_time;
}

// NOLINTEND(google-objc-function-naming,readability-identifier-naming)

#endif // #ifndef IMGUI_DISABLE

#endif
