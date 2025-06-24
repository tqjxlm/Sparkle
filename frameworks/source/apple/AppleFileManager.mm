#if FRAMEWORK_APPLE

#include "apple/AppleFileManager.h"

#include "core/Exception.h"
#include "core/Logger.h"

#import <Foundation/Foundation.h>
#include <filesystem>

namespace sparkle
{
std::unique_ptr<FileManager> FileManager::CreateNativeFileManager()
{
    ASSERT(!native_file_manager_);

    auto instance = std::make_unique<AppleFileManager>();
    native_file_manager_ = instance.get();
    return instance;
}

std::string AppleFileManager::GetAbosluteFilePath(const std::string &filepath, bool external)
{
    @autoreleasepool
    {
        NSError *error = nil;

#if FRAMEWORK_MACOS
        if (!external)
        {
            // NSApplicationSupportDirectory is not available in this case, use shared support instead
            NSString *app_support_dir = [[NSBundle mainBundle] sharedSupportPath];
            return std::string([app_support_dir UTF8String]) + "/" + filepath;
        }
#endif

        NSURL *document_dir = [[NSFileManager defaultManager]
              URLForDirectory:external ? NSDocumentDirectory : NSApplicationSupportDirectory
                     inDomain:NSUserDomainMask
            appropriateForURL:nil
                       create:YES
                        error:&error];
        if (error)
        {
            Log(Error, "Error accessing Application Support directory: {}", [error.localizedDescription UTF8String]);
            return "";
        }

        NSURL *file_url = [document_dir URLByAppendingPathComponent:[NSString stringWithUTF8String:filepath.c_str()]
                                                        isDirectory:NO];

        return [file_url fileSystemRepresentation];
    }
}

std::string AppleFileManager::GetResourceFilePath(const std::string &filepath)
{
    @autoreleasepool
    {
        std::filesystem::path fs_path(ResourceRoot + filepath);

        NSString *resource_ns = [NSString stringWithUTF8String:fs_path.stem().c_str()];
        NSString *type_ns = [NSString stringWithUTF8String:fs_path.extension().string().substr(1).c_str()];
        NSString *dir_ns = [NSString stringWithUTF8String:fs_path.parent_path().c_str()];

        NSBundle *bundle = [NSBundle mainBundle];

        NSString *path = [bundle pathForResource:resource_ns ofType:type_ns inDirectory:dir_ns];

        if (!path)
        {
            return "";
        }

        return [path UTF8String];
    }
}

bool AppleFileManager::ResourceExists(const std::string &filepath)
{
    return !GetResourceFilePath(filepath).empty();
}

size_t AppleFileManager::GetResourceSize(const std::string &filepath)
{
    @autoreleasepool
    {
        auto resource_path = GetResourceFilePath(filepath);
        if (!resource_path.empty())
        {
            NSURL *file_url = [NSURL fileURLWithPath:[NSString stringWithCString:resource_path.c_str()
                                                                        encoding:NSUTF8StringEncoding]];
            NSNumber *size_value = nil;
            NSError *error = nil;
            [file_url getResourceValue:&size_value forKey:NSURLFileSizeKey error:&error];
            if (size_value)
            {
                return size_value.longLongValue;
            }
        }

        return std::numeric_limits<size_t>::max();
    }
}

std::vector<char> AppleFileManager::ReadResource(const std::string &filepath)
{
    std::vector<char> loaded_data;

    @autoreleasepool
    {
        auto resource_path = GetResourceFilePath(filepath);
        if (resource_path.empty())
        {
            Log(Warn, "Failed to find resource file {}", filepath);
            return loaded_data;
        }
        NSString *path = [NSString stringWithUTF8String:resource_path.c_str()];

        NSError *error = nil;
        NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&error];

        if (!data)
        {
            Log(Warn, "Failed to read file {}", [error.localizedDescription UTF8String]);
            return loaded_data;
        }

        const auto *data_ptr = static_cast<const char *>(data.bytes);
        loaded_data = std::vector<char>(data_ptr, data_ptr + data.length);

        return loaded_data;
    }
}
} // namespace sparkle

#endif
