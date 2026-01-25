#if FRAMEWORK_ANDROID

#include "android/AndroidFileManager.h"
#include "core/Logger.h"

#include <android/asset_manager.h>

namespace sparkle
{
std::unique_ptr<FileManager> FileManager::CreateNativeFileManager()
{
    ASSERT(!native_file_manager_);

    auto instance = std::make_unique<AndroidFileManager>();
    native_file_manager_ = instance.get();
    return instance;
}

std::filesystem::path AndroidFileManager::ResolvePath(const Path &path)
{
    switch (path.type)
    {
    case PathType::Internal:
        return internal_file_path_ / path.path;
    case PathType::External:
        return external_file_path_ / path.path;
    default:
        UnImplemented(path.type);
        return {};
    }
}

bool AndroidFileManager::Exists(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        auto resource_path = ResourceRoot / file.path;
        AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
        bool exists = (asset != nullptr);
        if (asset != nullptr)
        {
            AAsset_close(asset);
        }
        return exists;
    }

    // default to use parent implementation
    return StdFileManager::Exists(file);
}

size_t AndroidFileManager::GetSize(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        auto resource_path = ResourceRoot / file.path;
        AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
        if (asset == nullptr)
        {
            return std::numeric_limits<size_t>::max();
        }

        auto size = AAsset_getLength64(asset);
        AAsset_close(asset);
        return size;
    }

    // default to use parent implementation
    return StdFileManager::GetSize(file);
}

std::vector<char> AndroidFileManager::Read(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        auto resource_path = ResourceRoot / file.path;
        AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_BUFFER);
        if (asset == nullptr)
        {
            return {};
        }

        auto size = AAsset_getLength64(asset);
        std::vector<char> buffer;
        buffer.resize(size);

        AAsset_read(asset, buffer.data(), size);
        AAsset_close(asset);

        return buffer;
    }

    // default to use parent implementation
    return StdFileManager::Read(file);
}

std::vector<Path> AndroidFileManager::ListDirectory(const Path &dirpath)
{
    if (dirpath.type == PathType::Resource)
    {
        std::vector<Path> entries;

        if (!asset_manager_)
        {
            return entries;
        }

        auto resource_path = ResourceRoot / dirpath.path;
        AAssetDir *asset_dir = AAssetManager_openDir(asset_manager_, resource_path.c_str());
        if (asset_dir == nullptr)
        {
            Log(Warn, "Failed to open asset dir {}", resource_path.string());
            return entries;
        }

        Log(Info, "Iterating asset dir {}", resource_path.string());

        const char *filename;
        while ((filename = AAssetDir_getNextFileName(asset_dir)) != nullptr)
        {
            Log(Info, "Found item in asset dir {}", filename);

            std::string full_path = resource_path / filename;
            entries.emplace_back(full_path, dirpath.type);
        }

        AAssetDir_close(asset_dir);
        return entries;
    }

    // default to use parent implementation
    return StdFileManager::ListDirectory(dirpath);
}

void AndroidFileManager::Setup(AAssetManager *asset_manager, const std::string &interal_file_path,
                               const std::string &external_file_path)
{
    asset_manager_ = asset_manager;
    internal_file_path_ = interal_file_path;
    external_file_path_ = external_file_path;
}
} // namespace sparkle

#endif
