#pragma once

#include "core/Exception.h"
#include "core/FileTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace sparkle
{
// an abstract class to handle platform-specific files
// files are managed by three types:
// 1. resource file. read-only. may need platform-specific methods to read.
// 2. internal file. read-write. not user-visible. can be handled generally with std::fstream.
// 3. external file. read-write. user-visible. can be handled generally with std::fstream.
class FileManager
{
public:
    // one and only one native file manager should be created,
    // which means this function should only be defined once.
    static std::unique_ptr<FileManager> CreateNativeFileManager();

    static FileManager *GetNativeFileManager()
    {
        ASSERT(native_file_manager_)
        return native_file_manager_;
    }

    static void DestroyNativeFileManager()
    {
        native_file_manager_ = nullptr;
    }

    virtual ~FileManager();

    virtual std::string GetAbsoluteFilePath(const FileEntry &file) = 0;
    virtual bool Exists(const FileEntry &file) = 0;
    virtual size_t GetSize(const FileEntry &file) = 0;
    virtual std::vector<char> Read(const FileEntry &file) = 0;
    virtual std::string Write(const FileEntry &file, const char *data, uint64_t size) = 0;

    std::string Write(const FileEntry &file, const std::vector<char> &data)
    {
        return Write(file, data.data(), data.size());
    }

    template <class T> T ReadAsType(const FileEntry &file);

    virtual bool TryCreateDirectory(const FileEntry &file) = 0;

    // List all files and directories in the given directory path
    virtual std::vector<PathEntry> ListDirectory(const FileEntry &dirpath) = 0;

protected:
    static FileManager *native_file_manager_;
    static const std::string ResourceRoot;
    static const std::string GeneratedRoot;
};
} // namespace sparkle
