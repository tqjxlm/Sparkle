#if FRAMEWORK_ANDROID

#include "android/AndroidFileManager.h"
#include "core/Logger.h"

#include <android/asset_manager.h>
#include <sstream>

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

        // Android's AAssetDir_getNextFileName() only returns files, not subdirectories.
        // We use a manifest file (_dir_manifest.txt) generated at build time to list subdirectories.
        auto manifest_path = resource_path / "_dir_manifest.txt";
        AAsset *manifest_asset = AAssetManager_open(asset_manager_, manifest_path.c_str(), AASSET_MODE_BUFFER);
        if (manifest_asset != nullptr)
        {
            auto size = AAsset_getLength64(manifest_asset);
            std::string manifest_content(static_cast<size_t>(size), '\0');
            AAsset_read(manifest_asset, manifest_content.data(), size);
            AAsset_close(manifest_asset);

            // Parse manifest - each line is a subdirectory name
            std::istringstream stream(manifest_content);
            std::string line;
            while (std::getline(stream, line))
            {
                if (!line.empty())
                {
                    entries.emplace_back(dirpath.path / line, dirpath.type);
                }
            }
        }

        // Also iterate files using standard API (AAssetDir only returns files, not directories)
        AAssetDir *asset_dir = AAssetManager_openDir(asset_manager_, resource_path.c_str());
        if (asset_dir == nullptr)
        {
            Log(Warn, "Failed to open asset dir {}", resource_path.string());
            return entries;
        }

        const char *filename;
        while ((filename = AAssetDir_getNextFileName(asset_dir)) != nullptr)
        {
            // Skip the manifest file itself
            if (std::string(filename) == "_dir_manifest.txt")
            {
                continue;
            }
            entries.emplace_back(dirpath.path / filename, dirpath.type);
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
