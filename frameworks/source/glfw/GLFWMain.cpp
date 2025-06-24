#if FRAMEWORK_GLFW

#include "application/AppFramework.h"
#include "core/CoreStates.h"
#include "glfw/GLFWNativeView.h"

int main(int argc, char *argv[])
{
    sparkle::GLFWNativeView view;

    sparkle::AppFramework app;
    // early init of core functionalities (logger, threading, config)
    app.InitCore(argc, argv);

    app.SetNativeView(&view);

    // now we have a native view for presenting, fully init the app
    if (!app.Init())
    {
        return 1;
    }

    while (!sparkle::CoreStates::IsExiting() && !view.ShouldClose())
    {
        app.MainLoop();
    }

    app.Cleanup();

    return 0;
}

#endif
