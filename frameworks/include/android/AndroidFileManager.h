#pragma once

#if FRAMEWORK_ANDROID

#include "io/StdFileManager.h"

#include <android/asset_manager_jni.h>

namespace sparkle
{
class AndroidFileManager : public StdFileManager
{
public:
    std::vector<char> ReadResource(const std::string &filepath) override;

    bool ResourceExists(const std::string &filepath) override;

    size_t GetResourceSize(const std::string &filepath) override;

    void Setup(AAssetManager *asset_manager, const std::string &interal_file_path,
               const std::string &external_file_path);

    std::string GetAbosluteFilePath(const std::string &filepath, bool external) override
    {
        return (external ? external_file_path_ : internal_file_path_) + "/" + filepath;
    };

    std::vector<PathEntry> ListResourceDirectory(const std::string &dirpath) override;

    // some libraryies may want to use asset manager directly
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
