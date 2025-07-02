#pragma once

#pragma region Platform Macros

/*
 * Platform macros are predefined by the compiler or the build system. Do not define them manually.
 *
 * PLATFORM_XXX: defines the underlying system of the platform.
 * FRAMEWORK_XXX: defines the framework for window management and event handling.
 */

#ifndef FRAMEWORK_GLFW
#define FRAMEWORK_GLFW 0
#endif

#ifndef FRAMEWORK_ANDROID
#define FRAMEWORK_ANDROID 0
#endif

#ifndef FRAMEWORK_MACOS
#define FRAMEWORK_MACOS 0
#endif

#ifndef FRAMEWORK_IOS
#define FRAMEWORK_IOS 0
#endif

#define FRAMEWORK_APPLE (FRAMEWORK_MACOS || FRAMEWORK_IOS)

#if defined(__APPLE__)
#if !(FRAMEWORK_GLFW || FRAMEWORK_APPLE)
#error "PLATFORM_APPLE only supports: FRAMEWORK_GLFW, FRAMEWORK_APPLE"
#endif
#define PLATFORM_APPLE 1
#else
#define PLATFORM_APPLE 0
#endif

#if defined(_WIN32) || defined(_WIN64)
#if !(FRAMEWORK_GLFW)
#error "PLATFORM_WINDOWS only supports: FRAMEWORK_GLFW"
#endif
#define PLATFORM_WINDOWS 1
#else
#define PLATFORM_WINDOWS 0
#endif

#if (defined(__linux__) || defined(__linux) || defined(linux) || defined(__gnu_linux__))
#define PLATFORM_LINUX 1
#else
#define PLATFORM_LINUX 0
#endif

#ifndef NDEBUG
#define DEBUG_BUILD 1
#else
#define DEBUG_BUILD 0
#endif

#pragma endregion
