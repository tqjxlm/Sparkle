#include "core/Path.h"

#include "core/FileManager.h"

namespace sparkle
{
std::filesystem::path Path::Resolved() const
{
    return FileManager::GetNativeFileManager()->ResolvePath(*this);
}
} // namespace sparkle
