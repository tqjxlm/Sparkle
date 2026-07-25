#include "io/HdrCubeTranscodeJob.h"

#include <crc32.h>

#include <array>

namespace sparkle
{
namespace
{
std::string TranscodeType(const std::string &master_type, TextureCompression::Family family)
{
    return TextureCompression::MakeFamilyType(master_type, family);
}
} // namespace

HdrCubeTranscodeJob::HdrCubeTranscodeJob(const std::string &master_type, TextureCompression::Family family,
                                         std::string source_name, std::vector<char> master_payload,
                                         uint32_t source_hash)
    : type_(TranscodeType(master_type, family)), source_name_(std::move(source_name)),
      master_payload_(std::move(master_payload)), family_(family), source_hash_(source_hash)
{
}

uint32_t HdrCubeTranscodeJob::MakeSourceHash(uint32_t origin_content_hash, uint32_t master_version)
{
    const std::array<uint32_t, 2> inputs{origin_content_hash, master_version};
    CRC32 hasher;
    hasher.add(inputs.data(), inputs.size() * sizeof(uint32_t));
    uint32_t hash = 0;
    hasher.getHash(reinterpret_cast<unsigned char *>(&hash));
    return hash;
}

CookArtifactKey HdrCubeTranscodeJob::MakeIdentityKey(const std::string &master_type, TextureCompression::Family family,
                                                     const std::string &source_name)
{
    return {.type = TranscodeType(master_type, family),
            .version = Version,
            .source_name = source_name,
            .source_hash = std::nullopt};
}

CookArtifactKey HdrCubeTranscodeJob::MakeKey(const std::string &master_type, TextureCompression::Family family,
                                             const std::string &source_name, uint32_t origin_content_hash,
                                             uint32_t master_version)
{
    auto key = MakeIdentityKey(master_type, family, source_name);
    key.source_hash = MakeSourceHash(origin_content_hash, master_version);
    return key;
}

CookJobResult HdrCubeTranscodeJob::Execute()
{
    auto payload = TextureCompression::TranscodeHdrCube(master_payload_, TextureCompression::SelectHdrFormat(family_));
    if (payload.empty())
    {
        return CookJobResult::Failure();
    }
    return CookJobResult::Success(std::move(payload));
}
} // namespace sparkle
