#pragma once

/*
 * Platform macros are predefined by the compiler or the build system. Do not define them manually.
 *
 * PLATFORM_XXX: defines the underlying system of the platform.
 * FRAMEWORK_XXX: defines the framework for window management and event handling.
 */

#pragma region Platform Macros

#ifndef PLATFORM_MACOS
#define PLATFORM_MACOS 0
#endif

#ifndef PLATFORM_IOS
#define PLATFORM_IOS 0
#endif

#ifndef PLATFORM_WINDOWS
#define PLATFORM_WINDOWS 0
#endif

#define PLATFORM_APPLE (PLATFORM_MACOS || PLATFORM_IOS)

#if (defined(__linux__) || defined(__linux) || defined(linux) || defined(__gnu_linux__))
#define PLATFORM_LINUX 1
#else
#define PLATFORM_LINUX 0
#endif

#pragma endregion

#pragma region Framework Macros

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

#ifndef PLATFORM_ANDROID
#define PLATFORM_ANDROID 0
#endif

#define FRAMEWORK_APPLE (FRAMEWORK_MACOS || FRAMEWORK_IOS)

#pragma endregion

#pragma region Platform Sanity Check

#if PLATFORM_MACOS
#if !(FRAMEWORK_GLFW || FRAMEWORK_MACOS)
#error "PLATFORM_MACOS only supports: FRAMEWORK_GLFW, FRAMEWORK_MACOS"
#endif
#endif

#if PLATFORM_IOS
#if !(FRAMEWORK_IOS)
#error "PLATFORM_IOS only supports: FRAMEWORK_IOS"
#endif
#endif

#if PLATFORM_WINDOWS
#if !(FRAMEWORK_GLFW)
#error "PLATFORM_WINDOWS only supports: FRAMEWORK_GLFW"
#endif
#endif

#if PLATFORM_ANDROID
#if !(FRAMEWORK_ANDROID)
#error "PLATFORM_ANDROID only supports: FRAMEWORK_ANDROID"
#endif
#endif

#pragma endregion

#ifndef NDEBUG
#define DEBUG_BUILD 1
#else
#define DEBUG_BUILD 0
#endif
