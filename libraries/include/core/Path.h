#pragma once

#include <filesystem>
#include <utility>

namespace sparkle
{
enum class PathType : std::uint8_t
{
    Resource, // read-only bundled files, may need platform-specific methods to read
    Internal, // read-write files, not user-visible if the platform allows for it
    External, // read-write files, user-visible
    Num       // invalid path
};

struct Path
{
    std::filesystem::path path;
    PathType type;

    Path() : type(PathType::Num)
    {
    }

    Path(std::filesystem::path filepath, PathType file_type) : path(std::move(filepath)), type(file_type)
    {
    }

    [[nodiscard]] bool IsValid() const
    {
        return type != PathType::Num;
    }

    // get a path recognized by std filesystem (may not be possible for a Resource path)
    [[nodiscard]] std::filesystem::path Resolved() const;

    static Path Resource(const std::filesystem::path &filepath)
    {
        return Path(filepath, PathType::Resource);
    }

    static Path Internal(const std::filesystem::path &filepath)
    {
        return Path(filepath, PathType::Internal);
    }

    static Path External(const std::filesystem::path &filepath)
    {
        return Path(filepath, PathType::External);
    }
};
} // namespace sparkle
