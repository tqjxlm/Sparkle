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
} // namespace sparkle

#endif
