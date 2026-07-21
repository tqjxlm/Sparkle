#include "core/cook/CookArtifactStore.h"

#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"

#include <crc32.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace sparkle
{
namespace
{
struct ArtifactHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t source_hash;
    uint32_t payload_size;
};

constexpr uint32_t ArtifactMagic = 0x4B4F4F43;
constexpr const char *ManifestFilePath = "cooked/manifest.json";

std::shared_mutex &GetManifestMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

std::string GetArtifactPath(const CookArtifactKey &key)
{
    CRC32 hasher;
    hasher.add(key.source_name.data(), key.source_name.size());
    uint32_t name_hash = 0;
    hasher.getHash(reinterpret_cast<unsigned char *>(&name_hash));

    const auto stem = std::filesystem::path(key.source_name).stem().string();
    return fmt::format("cooked/{}/{}_{:08x}.cook", key.type, stem, name_hash);
}

nlohmann::json ReadManifest(PathType domain)
{
    auto *file_manager = FileManager::GetNativeFileManager();

    Path path(ManifestFilePath, domain);
    if (!file_manager->Exists(path))
    {
        return nlohmann::json::object();
    }

    auto data = file_manager->Read(path);
    auto parsed = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

bool ValidateArtifact(const CookPayload &data, const CookArtifactKey &key)
{
    if (!key.source_hash || data.size() < sizeof(ArtifactHeader))
    {
        return false;
    }

    ArtifactHeader header;
    std::memcpy(&header, data.data(), sizeof(header));

    return header.magic == ArtifactMagic && header.version == key.version && header.source_hash == *key.source_hash &&
           header.payload_size == data.size() - sizeof(ArtifactHeader);
}

CookPayload StripHeader(CookPayload data)
{
    data.erase(data.begin(), data.begin() + sizeof(ArtifactHeader));
    return data;
}

// write-then-rename so crashes and concurrent readers never observe partial content
bool WriteFileAtomically(const std::string &relative_path, const char *data, size_t size)
{
    auto *file_manager = FileManager::GetNativeFileManager();

    const auto temp_path = relative_path + ".tmp";
    if (file_manager->Write(Path::Internal(temp_path), data, size).empty())
    {
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(file_manager->ResolvePath(Path::Internal(temp_path)),
                            file_manager->ResolvePath(Path::Internal(relative_path)), rename_error);
    return !rename_error;
}

CookPayload TryLoadManifestEntry(const nlohmann::json &entry, const CookArtifactKey &key, PathType path_type)
{
    if (!entry.is_object())
    {
        return {};
    }

    const auto entry_version = entry.value("version", 0u);
    const auto entry_hash = entry.value("source_hash", 0u);
    const auto artifact_file = entry.value("artifact", "");

    if (entry_version != key.version || (key.source_hash && entry_hash != *key.source_hash))
    {
        return {};
    }

    auto artifact_path = Path(artifact_file, path_type);
    if (!FileManager::GetNativeFileManager()->Exists(artifact_path))
    {
        return {};
    }

    auto data = FileManager::GetNativeFileManager()->Read(artifact_path);

    CookArtifactKey resolved_key = key;
    resolved_key.source_hash = entry_hash;

    if (data.empty() || !ValidateArtifact(data, resolved_key))
    {
        return {};
    }

    Log(Info, "cook artifact hit ({}): {}", path_type == PathType::Resource ? "packaged" : "cached", artifact_file);
    return StripHeader(std::move(data));
}

CookPayload LoadFromStores(const CookArtifactKey &key)
{
    const auto manifest_key = CookArtifactStore::GetManifestKey(key);

    for (auto path_type : {PathType::Resource, PathType::Internal})
    {
        const auto manifest = ReadManifest(path_type);

        const auto entry = manifest.find(manifest_key);
        if (entry != manifest.end())
        {
            if (auto payload = TryLoadManifestEntry(*entry, key, path_type); !payload.empty())
            {
                return payload;
            }
        }

        if (!key.source_hash)
        {
            continue;
        }

        const auto type_prefix = key.type + ":";
        for (auto candidate = manifest.begin(); candidate != manifest.end(); ++candidate)
        {
            if (candidate.key() == manifest_key || !candidate.key().starts_with(type_prefix))
            {
                continue;
            }

            if (auto payload = TryLoadManifestEntry(*candidate, key, path_type); !payload.empty())
            {
                return payload;
            }
        }
    }

    return {};
}
} // namespace

std::string CookArtifactStore::GetManifestKey(const CookArtifactKey &key)
{
    return key.type + ":" + key.source_name;
}

CookPayload CookArtifactStore::Load(const CookArtifactKey &key, bool ignore_rebuild_config)
{
    if (!ignore_rebuild_config)
    {
        auto *rebuild_config = ConfigManager::Instance().GetConfig<bool>("rebuild_cache");
        if (rebuild_config != nullptr && rebuild_config->Get())
        {
            return {};
        }
    }

    std::shared_lock<std::shared_mutex> lock(GetManifestMutex());
    return LoadFromStores(key);
}

bool CookArtifactStore::Save(const CookArtifactKey &key, const CookPayload &payload)
{
    if (!key.source_hash)
    {
        Log(Error, "cannot save unresolved cook artifact: {}:{}", key.type, key.source_name);
        return false;
    }
    if (payload.empty())
    {
        Log(Error, "cannot save empty cook artifact: {}:{}", key.type, key.source_name);
        return false;
    }
    if (payload.size() > std::numeric_limits<uint32_t>::max())
    {
        Log(Error, "cook artifact is too large: {}:{}", key.type, key.source_name);
        return false;
    }

    const ArtifactHeader header{.magic = ArtifactMagic,
                                .version = key.version,
                                .source_hash = *key.source_hash,
                                .payload_size = static_cast<uint32_t>(payload.size())};

    CookPayload data(sizeof(header) + payload.size());
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), payload.data(), payload.size());

    const auto path = GetArtifactPath(key);
    if (!WriteFileAtomically(path, data.data(), data.size()))
    {
        Log(Error, "failed to save cook artifact: {}", path);
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(GetManifestMutex());

        auto manifest = ReadManifest(PathType::Internal);
        manifest[GetManifestKey(key)] = {
            {"artifact", path}, {"version", key.version}, {"source_hash", *key.source_hash}};

        const auto serialized = manifest.dump(2);
        if (!WriteFileAtomically(ManifestFilePath, serialized.data(), serialized.size()))
        {
            Log(Error, "failed to update cook manifest for {}", path);
            return false;
        }
    }

    Log(Info, "saved cook artifact to {}", path);
    return true;
}
} // namespace sparkle
