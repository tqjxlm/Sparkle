# Test Guide

## Visual Test

* Use `--test_case screenshot` to take a screenshot after the scene is fully loaded and a frame is fully rendered.
* Use `--test_case multi_frame_screenshot` to take 5 frames of screenshots after the scene is fully loaded and a frame is fully rendered. This is useful for temporal analysis.
* Screenshot test cases work with `--headless true`, so it is suitable for commandline use.
* Screenshots are saved to [external-storage-path]/screenshots/. For `--test_case screenshot`, the file is named `screenshot.png`. For `--test_case multi_frame_screenshot`, files are named `multi_frame_N.png` where `N` is the frame index. The UI "Save Screenshot" button names files with the scene name, pipeline, and timestamp. For [external-storage-path], refer to [Run.md](Run.md).
* Ground truth images can be found in [CI.md](CI.md). But if you are working on a feature that is meant to change the final image output, you should not rely on the ground truth images.

## Python Test Scripts

* When possible, always use python scripts to perform tests.
* Make use of any necessary python libraries and add them to dev/requirements.txt
* General test scripts are stored at dev/.
* Dedicated test scripts are stored at tests/.
* If python is not sufficient to test your case, also make use of TestCase system below. They can be combined with python scripts freely. See dev/functional_test.py as an example.

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

Use `--test_timeout <frames>` to set a frame budget for the test. If the test does not
finish within the given number of frames it is reported as `Fail`. Default is `0` (no limit).

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

| Name                     | File                                                                                              | What it does                                                                                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `smoke`                  | [tests/smoke/SmokeTest.cpp](../tests/smoke/SmokeTest.cpp)                                         | Waits 2 frames then returns `Pass`. Verifies the full init and scene-load pipeline.                                                                             |
| `screenshot`             | [tests/screenshot/ScreenshotTest.cpp](../tests/screenshot/ScreenshotTest.cpp)                     | Waits for renderer ready, clears all existing screenshots optionally, captures one, then passes. Used by functional tests and visual QA.                        |
| `multi_frame_screenshot` | [tests/screenshot/MultiFrameScreenshotTest.cpp](../tests/screenshot/MultiFrameScreenshotTest.cpp) | Waits for renderer ready, clears all existing screenshots optionally, captures five, then passes. Used by functional tests and visual QA for temporal analysis. |
