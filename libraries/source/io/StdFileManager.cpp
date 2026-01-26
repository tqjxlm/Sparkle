#include "io/StdFileManager.h"

#if FRAMEWORK_GLFW
#include "core/Exception.h"
#endif
#include "core/Logger.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace sparkle
{
#if FRAMEWORK_GLFW
std::unique_ptr<FileManager> FileManager::CreateNativeFileManager()
{
    ASSERT(!native_file_manager_);

    auto instance = std::make_unique<StdFileManager>();
    native_file_manager_ = instance.get();
    return instance;
}
#endif

std::filesystem::path StdFileManager::ResolvePath(const Path &path)
{
    std::filesystem::path base_path;
    switch (path.type)
    {
    case PathType::Resource:
        base_path = ResourceRoot;
        break;
    case PathType::Internal:
    case PathType::External:
        base_path = GeneratedRoot;
        break;
    default:
        UnImplemented(path.type);
    }

    fs::path fs_path(base_path / path.path);
    return fs::absolute(fs_path);
}

bool StdFileManager::Exists(const Path &file)
{
    ASSERT(file.IsValid());
    return fs::exists(file.Resolved());
}

size_t StdFileManager::GetSize(const Path &file)
{
    if (!Exists(file))
    {
        return std::numeric_limits<size_t>::max();
    }

    return fs::file_size(file.Resolved());
}

std::vector<char> StdFileManager::Read(const Path &file)
{
    ASSERT(file.IsValid());
    return ReadFile(file.Resolved().string());
}

std::string StdFileManager::Write(const Path &file, const char *data, uint64_t size)
{
    if (file.type == PathType::Resource)
    {
        Log(Error, "Cannot write to resource file: {}", file.path.string());
        return "";
    }

    fs::path path(file.path);
    fs::path dir = path.parent_path();
    if (!dir.empty())
    {
        Path dir_file(dir.string(), file.type);
        TryCreateDirectory(dir_file);
    }

    const auto &full_path = file.Resolved();

    std::ofstream ofs(full_path, std::ios_base::binary);
    if (!ofs.is_open())
    {
        Log(Warn, "Saving failed: unable to create file {}.", full_path.string());
        return "";
    }

    ofs.exceptions(std::ostream::goodbit);
    ofs.write(data, static_cast<std::streamsize>(size));

    if (!ofs)
    {
        Log(Warn, "Saving failed: unable to write file {}.", full_path.string());
        return "";
    }

    return fs::absolute(full_path).string();
}

bool StdFileManager::TryCreateDirectory(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        Log(Error, "Cannot create directory in resource location: {}", file.path.string());
        return false;
    }

    auto absolute_path = file.Resolved();

    if (!fs::exists(absolute_path))
    {
        if (!fs::create_directories(absolute_path))
        {
            Log(Warn, "Unable to create directory {}.", absolute_path.string());
            return false;
        }
    }

    return true;
}

std::vector<Path> StdFileManager::ListDirectory(const Path &dirpath)
{
    std::vector<Path> entries;
    const auto absolute_path = dirpath.Resolved();

    if (!fs::exists(absolute_path) || !fs::is_directory(absolute_path))
    {
        Log(Warn, "Directory does not exist or is not a directory: {}", absolute_path.string());
        return entries;
    }

    try
    {
        for (const auto &entry : fs::directory_iterator(absolute_path))
        {
            entries.emplace_back(entry.path(), dirpath.type);
        }
    }
    catch (const fs::filesystem_error &e)
    {
        Log(Error, "Error listing directory {}: {}", absolute_path.string(), e.what());
    }

    return entries;
}

std::vector<char> StdFileManager::ReadFile(const std::string &absolute_path)
{
    std::ifstream file(absolute_path, std::ios::ate | std::ios::binary);

    if (!file.is_open())
    {
        Log(Warn, "reading file failed. cannot open {}", absolute_path);
        return {};
    }

    auto file_size = file.tellg();
    std::vector<char> buffer;
    buffer.resize(static_cast<uint64_t>(file_size));

    file.seekg(0);
    file.read(buffer.data(), file_size);

    file.close();

    return buffer;
}
} // namespace sparkle
