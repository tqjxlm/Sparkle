#import "imgui_impl_ios.h"
#include <imgui.h>

#if FRAMEWORK_IOS

#ifndef IMGUI_DISABLE

#include "core/Exception.h"
#import "iOSView.h"

#import <CoreFoundation/CoreFoundation.h>
#import <MetalKit/MTKView.h>
#include <UIKit/UIGestureRecognizer.h>
#include <UIKit/UIKit.h>

#include <cfloat>
#include <ctime>

static CFTimeInterval GetMachAbsoluteTimeInSeconds()
{
    return (CFTimeInterval)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
}

static CGPoint LocalViewToUiViewCoordinates(const ImGuiIO &io, const iOSView *view, const CGPoint &point)
{
    const float view_height = static_cast<float>(view.bounds.size.height);
    if (view_height <= 0.f)
    {
        return point;
    }

    const float scale = io.DisplaySize.y / view_height;
    return CGPointMake(static_cast<CGFloat>(point.x * scale), static_cast<CGFloat>(point.y * scale));
}

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
    tap_recognizer_.name = @"imgui_tap";
    tap_recognizer_.delegate = view_;
    [view_ addGestureRecognizer:tap_recognizer_];

    pan_recognizer_ = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    pan_recognizer_.name = @"imgui_pan";
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
    io.AddMousePosEvent(static_cast<float>(scaled_location.x), static_cast<float>(scaled_location.y));
    io.AddMouseButtonEvent(0, true);
    io.AddMouseButtonEvent(0, false);

    // reset mouse position to avoid hovering
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

- (void)handlePan:(UIPanGestureRecognizer *)recognizer
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);

    constexpr float PixelsPerWheelStep = 80.f;

    if (recognizer.state == UIGestureRecognizerStateBegan)
    {
        auto location = [recognizer locationInView:view_];
        auto scaled_location = LocalViewToUiViewCoordinates(io, view_, location);
        io.AddMousePosEvent(static_cast<float>(scaled_location.x), static_cast<float>(scaled_location.y));

        [recognizer setTranslation:CGPointMake(0.f, 0.f) inView:view_];
        return;
    }

    if (recognizer.state == UIGestureRecognizerStateChanged)
    {
        // Keep wheel and pointer-move events separated to avoid trickling-induced wheel latency.
        auto translation = [recognizer translationInView:view_];
        auto scaled_translation = LocalViewToUiViewCoordinates(io, view_, translation);
        const float wheel_x = static_cast<float>(scaled_translation.x) / PixelsPerWheelStep;
        const float wheel_y = static_cast<float>(scaled_translation.y) / PixelsPerWheelStep;
        if (wheel_x != 0.f || wheel_y != 0.f)
        {
            io.AddMouseWheelEvent(wheel_x, wheel_y);
        }

        [recognizer setTranslation:CGPointMake(0.f, 0.f) inView:view_];
        return;
    }

    if (recognizer.state == UIGestureRecognizerStateEnded || recognizer.state == UIGestureRecognizerStateCancelled ||
        recognizer.state == UIGestureRecognizerStateFailed)
    {
        // Avoid stale hover after a touch scroll ends.
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
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
