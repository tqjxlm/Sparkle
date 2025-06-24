#if PLATFORM_APPLE

#include "core/ThreadUtils.h"

#include <pthread.h>

namespace sparkle
{

void SetCurrentThreadName(const std::string &name)
{
    auto truncated = name.substr(0, 63);
    pthread_setname_np(truncated.data());
}

} // namespace sparkle

#endif
