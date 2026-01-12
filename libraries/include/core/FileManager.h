#pragma once

#include "core/Exception.h"

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

    virtual std::string GetAbosluteFilePath(const std::string &filepath, bool external) = 0;

    virtual bool ResourceExists(const std::string &filepath) = 0;

    virtual size_t GetResourceSize(const std::string &filepath) = 0;

    virtual bool TryCreateDirectory(const std::string &filepath, bool external) = 0;

    virtual bool FileExists(const std::string &filepath, bool external) = 0;

    virtual std::vector<char> ReadResource(const std::string &filepath) = 0;

    virtual std::vector<char> ReadFile(const std::string &filepath, bool external) = 0;

    virtual std::string WriteFile(const std::string &filepath, const char *data, uint64_t size, bool external) = 0;

    std::string WriteFile(const std::string &filepath, const std::vector<char> &data, bool external)
    {
        return WriteFile(filepath, data.data(), data.size(), external);
    }

    struct PathEntry
    {
        std::string name;      // File or directory name
        bool is_directory;     // True if entry is a directory
        uint64_t size;         // File size in bytes (0 for directories)
    };

    // List all files and directories in the given directory path
    // Returns empty vector if directory doesn't exist or can't be read
    virtual std::vector<PathEntry> ListDirectory(const std::string &dirpath, bool external) = 0;

    // List resource directory contents
    virtual std::vector<PathEntry> ListResourceDirectory(const std::string &dirpath) = 0;

    template <class T> T ReadResourceAsType(const std::string &filepath);

protected:
    static FileManager *native_file_manager_;
    static const std::string ResourceRoot;
    static const std::string GeneratedRoot;
};
} // namespace sparkle
