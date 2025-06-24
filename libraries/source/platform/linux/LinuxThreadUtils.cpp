#if PLATFORM_LINUX

#include "core/ThreadUtils.h"

#include <pthread.h>

namespace sparkle
{

void SetCurrentThreadName(const std::string &name)
{
    auto truncated = name.substr(0, 15);
    pthread_setname_np(pthread_self(), truncated.c_str());
}

} // namespace sparkle
#endif
