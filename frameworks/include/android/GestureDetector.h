#pragma once

#if FRAMEWORK_ANDROID

#include "core/math/Types.h"

#include <android/native_window_jni.h>
#include <android/sensor.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <vector>

namespace sparkle
{
constexpr int32_t DoubleTapTimeout = 300 * 1000000;
constexpr int32_t TapTimeout = 180 * 1000000;
constexpr int32_t DoubleTapSlop = 100;
constexpr int32_t TouchSlop = 8;

enum : uint8_t
{
    GESTURE_STATE_NONE = 0,
    GESTURE_STATE_START = 1,
    GESTURE_STATE_MOVE = 2,
    GESTURE_STATE_END = 4,
    GESTURE_STATE_ACTION = (GESTURE_STATE_START | GESTURE_STATE_END),
};

using GESTURE_STATE = int32_t;

class GestureDetector
{
protected:
    float dp_factor_;

public:
    GestureDetector();

    virtual ~GestureDetector();

    virtual void SetConfiguration(AConfiguration *config);

    virtual GESTURE_STATE Detect(const GameActivityMotionEvent *motion_event) = 0;
};

class TapDetector : public GestureDetector
{
private:
    int32_t down_pointer_id_;
    float down_x_;
    float down_y_;

public:
    TapDetector();

    GESTURE_STATE Detect(const GameActivityMotionEvent *motion_event) override;
};

class DoubletapDetector : public GestureDetector
{
private:
    TapDetector tap_detector_;
    int64_t last_tap_time_;
    float last_tap_x_;
    float last_tap_y_;

public:
    DoubletapDetector();

    GESTURE_STATE Detect(const GameActivityMotionEvent *motion_event) override;
    void SetConfiguration(AConfiguration *config) override;
};

class PinchDetector : public GestureDetector
{
private:
    static int32_t FindIndex(const GameActivityMotionEvent *event, int32_t id);
    const GameActivityMotionEvent *event_;
    std::vector<int32_t> vec_pointers_;

public:
    PinchDetector() = default;

    GESTURE_STATE Detect(const GameActivityMotionEvent *event) override;
    bool GetPointers(Vector2 &v1, Vector2 &v2);
    bool GetPointer(Vector2 &v);

    uint32_t GetNumPointers()
    {
        return vec_pointers_.size();
    }
};

class DragDetector : public GestureDetector
{
private:
    static int32_t FindIndex(const GameActivityMotionEvent *event, int32_t id);
    const GameActivityMotionEvent *event_{};
    std::vector<int32_t> vec_pointers_;

public:
    DragDetector() = default;

    GESTURE_STATE Detect(const GameActivityMotionEvent *event) override;
    bool GetPointer(Vector2 &v);
};

} // namespace sparkle

#endif
