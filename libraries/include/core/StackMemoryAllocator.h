#pragma once

#include "core/math/Utilities.h"

#include <array>

namespace sparkle
{
// A very simple allocator that allocates memory from a stack.
// NOTICE: it will not call constructor or destructor in any way, you should take care of that yourself
// NOTICE: it is not thread safe
class StackMemoryAllocator
{
    static constexpr unsigned Capacity = 16 * 1024 * 1024;
    static constexpr unsigned Alignment = 64;

public:
    void Reset()
    {
        allocated_ = 0;
    }

    template <class T> T *Allocate()
    {
        T *pointer = reinterpret_cast<T *>(&stack_[allocated_]);
        allocated_ = utilities::AlignAddress(allocated_ + sizeof(T), Alignment);
        return pointer;
    }

private:
    size_t allocated_;
    std::array<uint8_t, Capacity> stack_;
};
} // namespace sparkle
