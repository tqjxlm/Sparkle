#pragma once

#if FRAMEWORK_IOS

#include <imgui.h>

#ifndef IMGUI_DISABLE

@class MTKView;

// NOLINTBEGIN(google-objc-function-naming,readability-identifier-naming)

IMGUI_IMPL_API bool ImGui_ImplIOS_Init(MTKView *_Nonnull view);
IMGUI_IMPL_API void ImGui_ImplIOS_Shutdown();
IMGUI_IMPL_API void ImGui_ImplIOS_NewFrame(MTKView *_Nullable view);

// NOLINTEND(google-objc-function-naming,readability-identifier-naming)

#endif

#endif
