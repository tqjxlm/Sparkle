#pragma once

#include "core/Exception.h"
#include "core/Path.h"

#include <memory>
#include <string>
#include <vector>

namespace sparkle
{
// an abstract class to handle platform-specific files
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

    virtual std::filesystem::path ResolvePath(const Path &path) = 0;
    virtual bool Exists(const Path &file) = 0;
    virtual size_t GetSize(const Path &file) = 0;
    virtual std::vector<char> Read(const Path &file) = 0;
    virtual std::string Write(const Path &file, const char *data, uint64_t size) = 0;

    std::string Write(const Path &file, const std::vector<char> &data)
    {
        return Write(file, data.data(), data.size());
    }

    template <class T> T ReadAsType(const Path &file);

    virtual bool TryCreateDirectory(const Path &file) = 0;

    // List all files and directories in the given directory path. it will always return raw file path.
    [[nodiscard]] virtual std::vector<Path> ListDirectory(const Path &dirpath) = 0;

protected:
    static FileManager *native_file_manager_;
    static const std::filesystem::path ResourceRoot;
    static const std::filesystem::path GeneratedRoot;
};
} // namespace sparkle
