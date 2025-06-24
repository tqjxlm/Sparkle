#include "core/FileManager.h"

#include "core/Logger.h"

namespace sparkle
{
FileManager *FileManager::native_file_manager_ = nullptr;
const std::string FileManager::ResourceRoot("packed/");
const std::string FileManager::GeneratedRoot("generated/");

FileManager::~FileManager()
{
    Log(Debug, "FileManager destroyed");
}

template <> std::string FileManager::ReadResourceAsType(const std::string &filepath)
{
    auto data = ReadResource(filepath);
    std::string data_string(data.begin(), data.end());
    return data_string;
}
} // namespace sparkle
