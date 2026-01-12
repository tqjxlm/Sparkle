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

std::string AndroidFileManager::GetAbsoluteFilePath(const FileEntry &file)
{
    switch (file.type)
    {
    case FileType::Resource:
        return ResourceRoot + file.path;
    case FileType::Internal:
        return internal_file_path_ + "/" + file.path;
    case FileType::External:
        return external_file_path_ + "/" + file.path;
    }
    return "";
}

bool AndroidFileManager::Exists(const FileEntry &file)
{
    if (file.type == FileType::Resource)
    {
        const auto &resource_path = ResourceRoot + file.path;
        AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
        bool exists = (asset != nullptr);
        if (asset)
        {
            AAsset_close(asset);
        }
        return exists;
    }

    // For Internal and External, use parent implementation
    return StdFileManager::Exists(file);
}

size_t AndroidFileManager::GetSize(const FileEntry &file)
{
    if (file.type == FileType::Resource)
    {
        const auto &resource_path = ResourceRoot + file.path;
        AAsset *asset = AAssetManager_open(asset_manager_, resource_path.c_str(), AASSET_MODE_UNKNOWN);
        if (!asset)
        {
            return std::numeric_limits<size_t>::max();
        }

        auto size = AAsset_getLength64(asset);
        AAsset_close(asset);
        return size;
    }

    // For Internal and External, use parent implementation
    return StdFileManager::GetSize(file);
}

std::vector<char> AndroidFileManager::Read(const FileEntry &file)
{
    if (file.type == FileType::Resource)
    {
        const auto &resource_path = ResourceRoot + file.path;
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

    // For Internal and External, use parent implementation
    return StdFileManager::Read(file);
}

std::vector<PathEntry> AndroidFileManager::ListDirectory(const FileEntry &dirpath)
{
    if (dirpath.type == FileType::Resource)
    {
        std::vector<PathEntry> entries;

        if (!asset_manager_)
        {
            return entries;
        }

        const auto &resource_path = ResourceRoot + dirpath.path;
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

    // For Internal and External, use parent implementation
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
