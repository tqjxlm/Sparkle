#include "io/HdrCubeTranscodeJob.h"

#include <crc32.h>

namespace sparkle
{
namespace
{
std::string TranscodeType(const std::string &master_type, TextureCompression::Family family)
{
    return master_type + "_" + TextureCompression::GetFamilyName(family);
}
} // namespace

HdrCubeTranscodeJob::HdrCubeTranscodeJob(const std::string &master_type, TextureCompression::Family family,
                                         std::string source_name, std::vector<char> master_payload)
    : type_(TranscodeType(master_type, family)), source_name_(std::move(source_name)),
      master_payload_(std::move(master_payload)), family_(family)
{
    CRC32 hasher;
    hasher.add(master_payload_.data(), master_payload_.size());
    hasher.getHash(reinterpret_cast<unsigned char *>(&source_hash_));
}

CookArtifactKey HdrCubeTranscodeJob::MakeLookupKey(const std::string &master_type, TextureCompression::Family family,
                                                   const std::string &source_name)
{
    return {.type = TranscodeType(master_type, family),
            .version = Version,
            .source_name = source_name,
            .source_hash = std::nullopt};
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
