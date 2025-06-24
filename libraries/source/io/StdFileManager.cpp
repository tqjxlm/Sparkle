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

bool StdFileManager::ResourceExists(const std::string &filepath)
{
    auto resource_path = ResourceRoot + filepath;
    return fs::exists(resource_path);
}

size_t StdFileManager::GetResourceSize(const std::string &filepath)
{
    if (!ResourceExists(filepath))
    {
        return std::numeric_limits<size_t>::max();
    }
    auto resource_path = ResourceRoot + filepath;
    return fs::file_size(resource_path);
}

std::string StdFileManager::GetAbosluteFilePath(const std::string &filepath, bool)
{
    fs::path fs_path(GeneratedRoot + filepath);
    return fs::absolute(fs_path).string();
}

bool StdFileManager::FileExists(const std::string &filepath, bool external)
{
    auto absolute_path = GetAbosluteFilePath(filepath, external);
    return fs::exists(absolute_path);
}

bool StdFileManager::TryCreateDirectory(const std::string &filepath, bool external)
{
    auto absolute_path_string = GetAbosluteFilePath(filepath, external);

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

std::vector<char> StdFileManager::ReadResource(const std::string &filepath)
{
    auto resource_path = ResourceRoot + filepath;
    return ReadFile(resource_path);
}

std::vector<char> StdFileManager::ReadFile(const std::string &filepath, bool external)
{
    const auto &absolute_path = GetAbosluteFilePath(filepath, external);
    return ReadFile(absolute_path);
}

std::string StdFileManager::WriteFile(const std::string &filepath, const char *data, uint64_t size, bool external)
{
    fs::path path(filepath);
    fs::path dir = path.parent_path();
    TryCreateDirectory(dir.string(), external);

    const auto &full_path = GetAbosluteFilePath(filepath, external);

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
} // namespace sparkle
