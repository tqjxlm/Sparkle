#if FRAMEWORK_ANDROID

#include "android/AndroidNativeView.h"
#include "application/AppFramework.h"
#include "core/CoreStates.h"
#include "core/Logger.h"

#include <chrono>
#include <thread>

void android_main(android_app *app_state) // NOLINT
{
    sparkle::AndroidNativeView view(app_state);

    sparkle::AppFramework app;

    app.SetNativeView(&view);
    view.SetApp(&app);

    // poll the first event so we get window cmd callback
    while (!view.IsValid())
    {
        view.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // early init of core functionalities (logger, threading, config)
    app.InitCore(0, nullptr);

    // now we have a native view for presenting, fully init the app
    if (!app.Init())
    {
        Log(Error, "Failed to init. Exit now.");
        return;
    }

    while (!sparkle::CoreStates::IsExiting() && !view.ShouldClose())
    {
        app.MainLoop();
    }

    app.Cleanup();
}

#endif
