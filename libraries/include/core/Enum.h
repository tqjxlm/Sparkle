#pragma once

#include <magic_enum/magic_enum.hpp>

template <typename T>
concept EnumType = std::is_enum_v<T>;

namespace sparkle
{
template <auto T> constexpr const char *Enum2Str()
{
    return magic_enum::enum_name<T>().data();
}

template <EnumType T> const char *Enum2Str(T item)
{
    return magic_enum::enum_name<T>(item).data();
}

template <EnumType T> bool Str2Enum(const std::string &str, T &out_value)
{
    auto converted = magic_enum::enum_cast<T>(str, magic_enum::case_insensitive);

    if (!converted.has_value())
    {
        return false;
    }

    out_value = converted.value();

    return true;
}
} // namespace sparkle

// NOLINTBEGIN(bugprone-macro-parentheses)

#define RegisterEnumAsFlag(Flags)                                                                                      \
    inline Flags operator|(Flags lhs, Flags rhs)                                                                       \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        return static_cast<Flags>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));                        \
    }                                                                                                                  \
                                                                                                                       \
    inline bool operator&(Flags lhs, Flags rhs)                                                                        \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        return static_cast<bool>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));                         \
    }                                                                                                                  \
                                                                                                                       \
    inline Flags operator^(Flags lhs, Flags rhs)                                                                       \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        return static_cast<Flags>(static_cast<Underlying>(lhs) ^ static_cast<Underlying>(rhs));                        \
    }                                                                                                                  \
                                                                                                                       \
    inline Flags operator~(Flags flag)                                                                                 \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        return static_cast<Flags>(~static_cast<Underlying>(flag));                                                     \
    }                                                                                                                  \
                                                                                                                       \
    inline Flags &operator|=(Flags &lhs, Flags rhs)                                                                    \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        lhs = static_cast<Flags>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));                         \
        return lhs;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    inline Flags &operator&=(Flags &lhs, Flags rhs)                                                                    \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        lhs = static_cast<Flags>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));                         \
        return lhs;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    inline Flags &operator^=(Flags &lhs, Flags rhs)                                                                    \
    {                                                                                                                  \
        using Underlying = std::underlying_type_t<Flags>;                                                              \
        lhs = static_cast<Flags>(static_cast<Underlying>(lhs) ^ static_cast<Underlying>(rhs));                         \
        return lhs;                                                                                                    \
    }

// NOLINTEND(bugprone-macro-parentheses)
