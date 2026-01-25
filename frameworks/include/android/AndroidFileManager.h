#pragma once

#if FRAMEWORK_ANDROID

#include "io/StdFileManager.h"

#include <android/asset_manager_jni.h>

namespace sparkle
{
class AndroidFileManager : public StdFileManager
{
public:
    std::filesystem::path ResolvePath(const Path &path) override;
    bool Exists(const Path &file) override;
    size_t GetSize(const Path &file) override;
    std::vector<char> Read(const Path &file) override;
    std::vector<Path> ListDirectory(const Path &dirpath) override;

    void Setup(AAssetManager *asset_manager, const std::string &interal_file_path,
               const std::string &external_file_path);

    // some libraries may want to use asset manager directly
    AAssetManager *GetAssetManager()
    {
        return asset_manager_;
    }

private:
    AAssetManager *asset_manager_ = nullptr;
    std::filesystem::path external_file_path_;
    std::filesystem::path internal_file_path_;
};
} // namespace sparkle

#endif
