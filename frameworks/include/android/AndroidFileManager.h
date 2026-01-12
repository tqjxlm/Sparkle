#pragma once

#if FRAMEWORK_ANDROID

#include "io/StdFileManager.h"

#include <android/asset_manager_jni.h>

namespace sparkle
{
class AndroidFileManager : public StdFileManager
{
public:
    std::string GetAbsoluteFilePath(const FileEntry &file) override;
    bool Exists(const FileEntry &file) override;
    size_t GetSize(const FileEntry &file) override;
    std::vector<char> Read(const FileEntry &file) override;
    std::vector<PathEntry> ListDirectory(const FileEntry &dirpath) override;

    void Setup(AAssetManager *asset_manager, const std::string &interal_file_path,
               const std::string &external_file_path);

    // some libraries may want to use asset manager directly
    AAssetManager *GetAssetManager()
    {
        return asset_manager_;
    }

private:
    AAssetManager *asset_manager_ = nullptr;
    std::string external_file_path_;
    std::string internal_file_path_;
};
} // namespace sparkle

#endif
