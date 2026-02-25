# Test Guide

## TestCase System

* A `TestCase` is a C++ class that runs inside the app after the scene finishes loading.
* Each frame, `AppFramework` calls `Tick()`. When `Tick()` returns `Pass` or `Fail` the app
exits with code `0` or `1` respectively.

### Build

TestCase support is compiled in by default (`ENABLE_TEST_CASES=1`).
Use `--strip_test` to exclude it from the binary for production builds:

```bash
# Build without TestCase support (production)
python3 build.py --framework [glfw, macos] --strip_test
```

### Run

* Test cases are selected at runtime via the `test_case` cvar.
* It is usually recommended to run test cases in headless mode.

```bash
# Build & Run the smoke test (exits 0 on pass, 1 on fail)
python3 build.py --framework [glfw, macos] --run --test_case smoke --headless true
echo "Exit code: $?"
```

The `--test_case <name>` argument is passed straight through to the app's config system.
Any other cvars (e.g. `--scene`, `--pipeline`) work alongside it as normal.

### Exit Codes

| Code | Meaning                                           |
| ---- | ------------------------------------------------- |
| `0`  | `TestCase::Result::Pass` — test passed            |
| `1`  | `TestCase::Result::Fail` — test failed            |
| `1`  | Named test case was not registered (init failure) |

## Writing a Test Case

1. Create a `.cpp` file anywhere under `tests/`.
2. Subclass `sparkle::TestCase` and implement `Tick()`.
3. Register with `TestCaseRegistrar<T>` using a unique name string.

```cpp
// tests/my_feature/MyFeatureTest.cpp
#include "application/AppFramework.h"
#include "application/TestCase.h"

namespace sparkle
{
class MyFeatureTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        // Inspect app state each frame. Return Pending to keep running.
        if (frame_++ < 10)
        {
            return Result::Pending;
        }
        // Evaluate your condition here.
        return Result::Pass;
    }

private:
    uint32_t frame_ = 0;
};

static TestCaseRegistrar<MyFeatureTest> my_feature_test_registrar("my_feature");
} // namespace sparkle
```

Then run it:

```bash
python3 build.py --framework macos --run --test_case my_feature
```

### Naming Rules

Test case names (the string passed to `TestCaseRegistrar`) must be unique across all
`.cpp` files under `tests/`. Duplicate names are detected at startup and logged as an
error; the first registration wins.

## Built-in Test Cases

| Name         | File                                                                          | What it does                                                                                                                  |
| ------------ | ----------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `smoke`      | [tests/smoke/SmokeTest.cpp](../tests/smoke/SmokeTest.cpp)                     | Waits 2 frames then returns `Pass`. Verifies the full init and scene-load pipeline.                                           |
| `screenshot` | [tests/screenshot/ScreenshotTest.cpp](../tests/screenshot/ScreenshotTest.cpp) | Waits for renderer ready, clears all existing screenshots in the screenshots directory, captures one, then passes. Used by functional tests and visual QA. |
