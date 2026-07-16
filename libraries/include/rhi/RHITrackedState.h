#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace sparkle
{
// remembers the bit pattern of a state last recorded into a command stream so redundant records
// can be dropped. keys must be trivially copyable and zero-initialized before filling, so padding
// bytes compare deterministically. comparison is bit-exact: a false mismatch only costs a record,
// a false match would render wrong, so NaN payloads and negative zero are distinct on purpose.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class RHITrackedState
{
public:
    // records value and returns true when it differs from what the command stream already has
    bool Update(const T &value)
    {
        const auto bits = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        if (valid_ && bits == bits_)
        {
            return false;
        }

        bits_ = bits;
        valid_ = true;
        return true;
    }

    void Reset()
    {
        valid_ = false;
    }

private:
    std::array<std::byte, sizeof(T)> bits_{};
    bool valid_ = false;
};
} // namespace sparkle
