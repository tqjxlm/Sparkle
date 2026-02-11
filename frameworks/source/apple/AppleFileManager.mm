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

std::filesystem::path AppleFileManager::ResolvePath(const Path &path)
{
    if (path.type != PathType::Internal && path.type != PathType::External)
    {
        UnImplemented(Enum2Str(path.type));
        return {};
    }

    // For Internal and External, use platform-specific paths
    @autoreleasepool
    {
        NSError *error = nil;

#if FRAMEWORK_MACOS
        if (path.type == PathType::Internal)
        {
            // NSApplicationSupportDirectory is not available in this case, use shared support instead
            NSString *app_support_dir = [[NSBundle mainBundle] sharedSupportPath];
            return std::filesystem::path([app_support_dir UTF8String]) / path.path;
        }
#endif

        bool is_external = path.type == PathType::External;
        NSURL *base_dir = [[NSFileManager defaultManager]
              URLForDirectory:is_external ? NSDocumentDirectory : NSApplicationSupportDirectory
                     inDomain:NSUserDomainMask
            appropriateForURL:nil
                       create:YES
                        error:&error];
        if (error)
        {
            Log(Error, "Error accessing directory: {}", [error.localizedDescription UTF8String]);
            return {};
        }

#if FRAMEWORK_MACOS
        // On macOS, scope external files under ~/Documents/<AppName>/ to avoid cluttering the Documents root
        if (is_external)
        {
            NSString *bundle_name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
            base_dir = [base_dir URLByAppendingPathComponent:bundle_name isDirectory:YES];
            NSError *dir_error = nil;
            [[NSFileManager defaultManager] createDirectoryAtURL:base_dir
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:&dir_error];
            if (dir_error)
            {
                Log(Error, "Failed to create external directory: {}", [dir_error.localizedDescription UTF8String]);
                return {};
            }
        }
#endif

        NSURL *file_url = [base_dir URLByAppendingPathComponent:[NSString stringWithUTF8String:path.path.c_str()]
                                                    isDirectory:NO];

        return [file_url fileSystemRepresentation];
    }
}

bool AppleFileManager::Exists(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        return !GetResourceFilePath(file.path).empty();
    }

    // For Internal and External, use parent implementation
    return StdFileManager::Exists(file);
}

size_t AppleFileManager::GetSize(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        @autoreleasepool
        {
            auto resource_path = GetResourceFilePath(file.path);
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

    // For Internal and External, use parent implementation
    return StdFileManager::GetSize(file);
}

std::vector<char> AppleFileManager::Read(const Path &file)
{
    if (file.type == PathType::Resource)
    {
        std::vector<char> loaded_data;

        @autoreleasepool
        {
            auto resource_path = GetResourceFilePath(file.path);
            if (resource_path.empty())
            {
                Log(Warn, "Failed to find resource file {}", file.path.string());
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

    // For Internal and External, use parent implementation
    return StdFileManager::Read(file);
}

std::vector<Path> AppleFileManager::ListDirectory(const Path &dirpath)
{
    if (dirpath.type == PathType::Resource)
    {
        std::vector<Path> entries;

        @autoreleasepool
        {
            std::filesystem::path fs_path(ResourceRoot / dirpath.path);
            NSString *dir_ns = [NSString stringWithUTF8String:fs_path.c_str()];

            NSBundle *bundle = [NSBundle mainBundle];
            NSString *bundle_resource_path = [bundle resourcePath];
            NSString *full_dir_path = [bundle_resource_path stringByAppendingPathComponent:dir_ns];

            NSFileManager *file_manager = [NSFileManager defaultManager];
            NSError *error = nil;

            BOOL is_directory = NO;
            if (![file_manager fileExistsAtPath:full_dir_path isDirectory:&is_directory] || !is_directory)
            {
                Log(Warn, "Resource directory does not exist or is not a directory: {}", [full_dir_path UTF8String]);
                return entries;
            }

            NSArray *contents = [file_manager contentsOfDirectoryAtPath:full_dir_path error:&error];

            if (error)
            {
                Log(Error, "Error listing resource directory {}: {}", [full_dir_path UTF8String],
                    [error.localizedDescription UTF8String]);
                return entries;
            }

            for (NSString *item in contents)
            {
                entries.emplace_back(dirpath.path / [item UTF8String], dirpath.type);
            }
        }

        return entries;
    }

    // For Internal and External, use parent implementation
    return StdFileManager::ListDirectory(dirpath);
}

std::string AppleFileManager::GetResourceFilePath(const std::string &filepath)
{
    @autoreleasepool
    {
        std::filesystem::path fs_path(ResourceRoot / filepath);

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
} // namespace sparkle

#endif
