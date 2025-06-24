#ifndef NDEBUG

#include "core/Exception.h"

#include <cpptrace/cpptrace.hpp>

#if PLATFORM_WINDOWS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonportable-system-include-path"
#include <Windows.h> // NOLINT
#pragma clang diagnostic pop
#endif

namespace sparkle
{
static void SignalDump(int signum)
{
    ExceptionHandler::PrintStackTrace();

    Logger::Flush();

#if PLATFORM_WINDOWS
    // windows does not have a way to break into debugger from signal handler, so we do it manually
    __debugbreak();
#endif

    // recover signal to avoid recursive dumping
    signal(signum, SIG_DFL);

    raise(signum);
}

#if PLATFORM_WINDOWS
static LONG WINAPI VectoredHandler(_EXCEPTION_POINTERS *ExceptionInfo)
{
    switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_STACK_OVERFLOW:
        Log(Error, "Windows exception: {:x}", ExceptionInfo->ExceptionRecord->ExceptionCode);

        SignalDump(SIGABRT);

        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
#endif

ExceptionHandler::ExceptionHandler()
{
    if (instanced_)
    {
        ASSERT_F(false, "Don't instantiate twice.");
        return;
    }

#if PLATFORM_WINDOWS
    AddVectoredExceptionHandler(1, VectoredHandler); // `1` = call this handler first
#else
    signal(SIGSEGV, &SignalDump);
    signal(SIGBUS, &SignalDump);
#endif
    signal(SIGABRT, &SignalDump);

    printf("Overrode signals\n");

    instanced_ = true;
}

static auto GetStackTraceInternal()
{
#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__) || defined(__unix)
    std::size_t skip_frame_count = 2;
#else
    std::size_t skip_frame_count = 4;
#endif
    auto &&trace_stack = cpptrace::generate_trace(skip_frame_count);

    // make the call stack cleaner
    auto should_exclude_frame = [](const cpptrace::stacktrace_frame &item) {
        return item.filename.ends_with("shared_ptr.h") || item.filename.ends_with("allocator_traits.h") ||
               item.filename.ends_with("construct_at.h") ||
               item.filename.find("libVkLayer_khronos_validation") != std::string::npos;
    };

    trace_stack.frames.erase(std::ranges::remove_if(trace_stack, should_exclude_frame).begin(), trace_stack.end());

    return trace_stack;
}

void ExceptionHandler::PrintStackTrace()
{
    Log(Error, "{}", GetStackTraceInternal().to_string(false));
}

std::string ExceptionHandler::GetStackTrace()
{
    return GetStackTraceInternal().to_string(false);
}

bool ExceptionHandler::instanced_ = false;

// we want this to be intialized as early as possible, so make it a static instance.
// no other module can actually mess up with its initialization, so there will not be initialization order issue.
static ExceptionHandler exception_handler;
} // namespace sparkle

#endif
