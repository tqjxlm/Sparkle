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

std::string StdFileManager::GetAbsoluteFilePath(const FileEntry &file)
{
    std::string base_path;
    switch (file.type)
    {
    case FileType::Resource:
        base_path = ResourceRoot;
        break;
    case FileType::Internal:
    case FileType::External:
        base_path = GeneratedRoot;
        break;
    }

    fs::path fs_path(base_path + file.path);
    return fs::absolute(fs_path).string();
}

bool StdFileManager::Exists(const FileEntry &file)
{
    if (file.type == FileType::Resource)
    {
        auto resource_path = ResourceRoot + file.path;
        return fs::exists(resource_path);
    }

    auto absolute_path = GetAbsoluteFilePath(file);
    return fs::exists(absolute_path);
}

size_t StdFileManager::GetSize(const FileEntry &file)
{
    if (!Exists(file))
    {
        return std::numeric_limits<size_t>::max();
    }

    auto absolute_path = GetAbsoluteFilePath(file);
    return fs::file_size(absolute_path);
}

std::vector<char> StdFileManager::Read(const FileEntry &file)
{
    auto absolute_path = GetAbsoluteFilePath(file);
    return ReadFile(absolute_path);
}

std::string StdFileManager::Write(const FileEntry &file, const char *data, uint64_t size)
{
    if (file.type == FileType::Resource)
    {
        Log(Error, "Cannot write to resource file: {}", file.path);
        return "";
    }

    fs::path path(file.path);
    fs::path dir = path.parent_path();
    if (!dir.empty())
    {
        FileEntry dir_file(dir.string(), file.type);
        TryCreateDirectory(dir_file);
    }

    const auto &full_path = GetAbsoluteFilePath(file);

    std::ofstream ofs(full_path, std::ios_base::binary);
    if (!ofs.is_open())
    {
        Log(Warn, "Saving failed: unable to create file {}.", full_path);
        return "";
    }

    ofs.exceptions(std::ostream::goodbit);
    ofs.write(data, static_cast<std::streamsize>(size));

    if (!ofs)
    {
        Log(Warn, "Saving failed: unable to write file {}.", full_path);
        return "";
    }

    return fs::absolute(full_path).string();
}

bool StdFileManager::TryCreateDirectory(const FileEntry &file)
{
    if (file.type == FileType::Resource)
    {
        Log(Error, "Cannot create directory in resource location: {}", file.path);
        return false;
    }

    auto absolute_path_string = GetAbsoluteFilePath(file);
    fs::path absolute_path(absolute_path_string);

    if (!fs::exists(absolute_path))
    {
        if (!fs::create_directories(absolute_path))
        {
            Log(Warn, "Unable to create directory {}.", absolute_path_string);
            return false;
        }
    }

    return true;
}

std::vector<PathEntry> StdFileManager::ListDirectory(const FileEntry &dirpath)
{
    std::vector<PathEntry> entries;
    const auto absolute_path = GetAbsoluteFilePath(dirpath);

    if (!fs::exists(absolute_path) || !fs::is_directory(absolute_path))
    {
        Log(Warn, "Directory does not exist or is not a directory: {}", absolute_path);
        return entries;
    }

    try
    {
        for (const auto &entry : fs::directory_iterator(absolute_path))
        {
            PathEntry dir_entry;
            dir_entry.name = entry.path().filename().string();
            dir_entry.is_directory = entry.is_directory();
            dir_entry.size = entry.is_regular_file() ? entry.file_size() : 0;
            entries.push_back(dir_entry);
        }
    }
    catch (const fs::filesystem_error &e)
    {
        Log(Error, "Error listing directory {}: {}", absolute_path, e.what());
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
