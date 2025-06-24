#if FRAMEWORK_ANDROID

#include "android/GestureDetector.h"
#include "core/Logger.h"

namespace sparkle
{
GestureDetector::GestureDetector()
{
    dp_factor_ = 1.f;
}

GestureDetector::~GestureDetector() = default;

void GestureDetector::SetConfiguration(AConfiguration *config)
{
    dp_factor_ = 160.f / AConfiguration_getDensity(config);
}

TapDetector::TapDetector() : down_x_(0), down_y_(0)
{
}

GESTURE_STATE TapDetector::Detect(const GameActivityMotionEvent *event)
{
    if (event->pointerCount > 1)
    {
        // Only support single touch
        return 0;
    }

    int32_t action = event->action;
    unsigned int flags = action & AMOTION_EVENT_ACTION_MASK;
    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN:
        down_pointer_id_ = event->pointers[0].id;
        down_x_ = event->pointers[0].rawX;
        down_y_ = event->pointers[0].rawY;
        break;
    case AMOTION_EVENT_ACTION_UP: {
        int64_t event_time = event->eventTime;
        int64_t down_time = event->downTime;
        if (event_time - down_time <= TapTimeout)
        {
            if (down_pointer_id_ == event->pointers[0].id)
            {
                float x = event->pointers[0].rawX - down_x_;
                float y = event->pointers[0].rawY - down_y_;
                if (x * x + y * y < TouchSlop * TouchSlop * dp_factor_)
                {
                    Log(Info, "TapDetector: Tap detected");
                    return GESTURE_STATE_ACTION;
                }
            }
        }
    }
    break;
    default:
        break;
    }
    return GESTURE_STATE_NONE;
}

DoubletapDetector::DoubletapDetector() : last_tap_time_(0), last_tap_x_(0), last_tap_y_(0)
{
}

GESTURE_STATE DoubletapDetector::Detect(const GameActivityMotionEvent *event)
{
    if (event->pointerCount > 1)
    {
        // Only support single double tap
        return 0;
    }

    bool tap_detected = tap_detector_.Detect(event) != 0;

    int32_t action = event->action;
    unsigned int flags = action & AMOTION_EVENT_ACTION_MASK;
    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN: {
        int64_t event_time = event->eventTime;
        if (event_time - last_tap_time_ <= DoubleTapTimeout)
        {
            float x = event->pointers[0].rawX - last_tap_x_;
            float y = event->pointers[0].rawY - last_tap_y_;
            if (x * x + y * y < DoubleTapSlop * DoubleTapSlop * dp_factor_)
            {
                Log(Info, "DoubletapDetector: Doubletap detected");
                return GESTURE_STATE_ACTION;
            }
        }
        break;
    }
    case AMOTION_EVENT_ACTION_UP:
        if (tap_detected)
        {
            last_tap_time_ = event->eventTime;
            last_tap_x_ = event->pointers[0].rawX;
            last_tap_y_ = event->pointers[0].rawY;
        }
        break;
    default:
        break;
    }
    return GESTURE_STATE_NONE;
}

void DoubletapDetector::SetConfiguration(AConfiguration *config)
{
    dp_factor_ = 160.f / AConfiguration_getDensity(config);
    tap_detector_.SetConfiguration(config);
}

int32_t PinchDetector::FindIndex(const GameActivityMotionEvent *event, int32_t id)
{
    int32_t count = event->pointerCount;
    for (auto i = 0; i < count; ++i)
    {
        if (id == event->pointers[i].id)
            return i;
    }
    return -1;
}

GESTURE_STATE PinchDetector::Detect(const GameActivityMotionEvent *event)
{
    GESTURE_STATE ret = GESTURE_STATE_NONE;
    int32_t action = event->action;
    uint32_t flags = action & AMOTION_EVENT_ACTION_MASK;
    event_ = event;

    int32_t count = event->pointerCount;
    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN:
        vec_pointers_.push_back(event->pointers[0].id);
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        int32_t pointer_index =
            (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        vec_pointers_.push_back(event->pointers[pointer_index].id);
        if (count == 2)
        {
            // Start new pinch
            ret = GESTURE_STATE_START;
        }
    }
    break;
    case AMOTION_EVENT_ACTION_UP:
        vec_pointers_.pop_back();
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP: {
        int32_t pointer_index =
            (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        int32_t released_pointer_id = event->pointers[pointer_index].id;

        auto it = vec_pointers_.begin();
        auto it_end = vec_pointers_.end();
        int32_t i = 0;
        for (; it != it_end; ++it, ++i)
        {
            if (*it == released_pointer_id)
            {
                vec_pointers_.erase(it);
                break;
            }
        }

        if (vec_pointers_.size() < 2)
        {
            ret = GESTURE_STATE_END;
        }
    }
    break;
    case AMOTION_EVENT_ACTION_MOVE:
        switch (count)
        {
        case 1:
            break;
        default:
            // Multi touch
            ret = GESTURE_STATE_MOVE;
            break;
        }
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        ret = GESTURE_STATE_END;
        break;
    default:
        break;
    }

    return ret;
}

bool PinchDetector::GetPointers(Vector2 &v1, Vector2 &v2)
{
    if (vec_pointers_.size() < 2)
        return false;

    int32_t pointer_index = FindIndex(event_, vec_pointers_[0]);
    if (pointer_index == -1)
        return false;

    float x = event_->pointers[pointer_index].rawX;
    float y = event_->pointers[pointer_index].rawY;

    pointer_index = FindIndex(event_, vec_pointers_[1]);
    if (pointer_index == -1)
        return false;

    float x2 = event_->pointers[pointer_index].rawX;
    float y2 = event_->pointers[pointer_index].rawY;

    v1 = Vector2(x, y);
    v2 = Vector2(x2, y2);

    return true;
}

bool PinchDetector::GetPointer(Vector2 &v)
{
    if (vec_pointers_.empty())
        return false;

    int32_t pointer_index = FindIndex(event_, vec_pointers_[0]);
    if (pointer_index == -1)
        return false;

    float x = event_->pointers[pointer_index].rawX;
    float y = event_->pointers[pointer_index].rawY;

    v = Vector2(x, y);

    return true;
}

int32_t DragDetector::FindIndex(const GameActivityMotionEvent *event, int32_t id)
{
    int32_t count = event->pointerCount;
    for (auto i = 0; i < count; ++i)
    {
        if (id == event->pointers[i].id)
            return i;
    }
    return -1;
}

GESTURE_STATE DragDetector::Detect(const GameActivityMotionEvent *event)
{
    GESTURE_STATE ret = GESTURE_STATE_NONE;
    int32_t action = event->action;
    int32_t pointer_index =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    uint32_t flags = action & AMOTION_EVENT_ACTION_MASK;
    event_ = event;

    int32_t count = event->pointerCount;
    switch (flags)
    {
    case AMOTION_EVENT_ACTION_DOWN:
        vec_pointers_.push_back(event->pointers[0].id);
        ret = GESTURE_STATE_START;
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        // when we have more than one pointers, it is not dragging anymore
        vec_pointers_.push_back(event->pointers[pointer_index].id);
        ret = GESTURE_STATE_END;
        break;
    case AMOTION_EVENT_ACTION_UP:
        vec_pointers_.pop_back();
        ret = GESTURE_STATE_END;
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP: {
        int32_t released_pointer_id = event->pointers[pointer_index].id;

        auto it = vec_pointers_.begin();
        auto it_end = vec_pointers_.end();
        int32_t i = 0;
        for (; it != it_end; ++it, ++i)
        {
            if (*it == released_pointer_id)
            {
                vec_pointers_.erase(it);
                break;
            }
        }

        if (i <= 1)
        {
            // Reset pinch or drag
            if (count == 2)
            {
                ret = GESTURE_STATE_START;
            }
        }
    }
    break;
    case AMOTION_EVENT_ACTION_MOVE: {
        switch (count)
        {
        case 1:
            // Drag
            ret = GESTURE_STATE_MOVE;
            break;
        default:
            break;
        }
    }
    break;
    case AMOTION_EVENT_ACTION_CANCEL:
    default:
        break;
    }

    return ret;
}

bool DragDetector::GetPointer(Vector2 &v)
{
    if (vec_pointers_.empty())
        return false;

    int32_t pointer_index = FindIndex(event_, vec_pointers_[0]);
    if (pointer_index == -1)
        return false;

    float x = event_->pointers[pointer_index].rawX;
    float y = event_->pointers[pointer_index].rawY;

    v = Vector2(x, y);

    return true;
}
} // namespace sparkle

#endif
