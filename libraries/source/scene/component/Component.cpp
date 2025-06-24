#include "scene/component/Component.h"

namespace sparkle
{
void Component::Tick()
{
    is_dirty_ = false;
}
} // namespace sparkle
