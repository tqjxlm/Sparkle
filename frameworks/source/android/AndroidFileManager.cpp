#if FRAMEWORK_ANDROID

#include "android/AndroidFileManager.h"

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

std::vector<char> AndroidFileManager::ReadResource(const std::string &filepath)
{
    const auto &resource_path = ResourceRoot + filepath;

    AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_BUFFER);
    if (!asset)
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

size_t AndroidFileManager::GetResourceSize(const std::string &filepath)
{
    const auto &resource_path = ResourceRoot + filepath;

    AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
    if (!asset)
    {
        return std::numeric_limits<size_t>::max();
    }

    auto size = AAsset_getLength64(asset);

    AAsset_close(asset);

    return size;
}

bool AndroidFileManager::ResourceExists(const std::string &filepath)
{
    const auto &resource_path = ResourceRoot + filepath;

    AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
    bool exists = (asset != nullptr);
    if (asset)
    {
        AAsset_close(asset);
    }
    return exists;
}

void AndroidFileManager::Setup(AAssetManager *asset_manager, const std::string &interal_file_path,
                               const std::string &external_file_path)
{
    asset_manager_ = asset_manager;
    internal_file_path_ = interal_file_path;
    external_file_path_ = external_file_path;
}

std::vector<FileManager::PathEntry> AndroidFileManager::ListResourceDirectory(const std::string &dirpath)
{
    std::vector<PathEntry> entries;

    if (!asset_manager_)
    {
        return entries;
    }

    const auto &resource_path = ResourceRoot + dirpath;

    AAssetDir *asset_dir = AAssetManager_openDir(asset_manager_, resource_path.c_str());
    if (!asset_dir)
    {
        return entries;
    }

    const char *filename;
    while ((filename = AAssetDir_getNextFileName(asset_dir)) != nullptr)
    {
        PathEntry entry;
        entry.name = filename;

        // Check if it's a directory by trying to open it as a directory
        std::string full_path = resource_path + "/" + filename;
        AAssetDir *test_dir = AAssetManager_openDir(asset_manager_, full_path.c_str());
        entry.is_directory = (test_dir != nullptr && AAssetDir_getNextFileName(test_dir) != nullptr);
        if (test_dir)
        {
            AAssetDir_close(test_dir);
        }

        if (!entry.is_directory)
        {
            // Get file size
            AAsset *asset = AAssetManager_open(asset_manager_, full_path.c_str(), AASSET_MODE_UNKNOWN);
            if (asset)
            {
                entry.size = AAsset_getLength64(asset);
                AAsset_close(asset);
            }
            else
            {
                entry.size = 0;
            }
        }
        else
        {
            entry.size = 0;
        }

        entries.push_back(entry);
    }

    AAssetDir_close(asset_dir);

    return entries;
}
} // namespace sparkle

#endif
