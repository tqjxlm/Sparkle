#pragma once

#if ENABLE_TEST_CASES

#include <stdexcept>

namespace sparkle
{
// a render graph error, thrown instead of aborting on a thread where an RGErrorsThrow lives
class RGError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// while one lives, render graph errors on the calling thread throw RGError instead of aborting, so a test can make
// them on purpose
class RGErrorsThrow
{
public:
    RGErrorsThrow()
    {
        throwing_ = true;
    }

    ~RGErrorsThrow()
    {
        throwing_ = previous_;
    }

    RGErrorsThrow(const RGErrorsThrow &) = delete;
    RGErrorsThrow &operator=(const RGErrorsThrow &) = delete;

    [[nodiscard]] static bool IsActive()
    {
        return throwing_;
    }

private:
    static inline thread_local bool throwing_ = false;
    bool previous_ = throwing_;
};
} // namespace sparkle

#endif // ENABLE_TEST_CASES
