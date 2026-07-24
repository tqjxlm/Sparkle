#pragma once

#include "core/cook/CookJob.h"
#include "io/TextureCompression.h"

namespace sparkle
{
// re-encodes an fp16 HDR cube master artifact into one family's HDR format. masters carry
// the expensive content (sky projection, IBL integration) in the shared pool; transcodes
// are the cheap per-family artifacts that ship in target images. the artifact type is the
// master's type suffixed with the family name (e.g. skylight_astc)
class HdrCubeTranscodeJob : public CookJob
{
public:
    static constexpr uint32_t Version = 3;

    HdrCubeTranscodeJob(const std::string &master_type, TextureCompression::Family family, std::string source_name,
                        std::vector<char> master_payload);

    // identity-only key for runtime artifact lookups
    [[nodiscard]] static CookArtifactKey MakeLookupKey(const std::string &master_type,
                                                       TextureCompression::Family family,
                                                       const std::string &source_name);

    [[nodiscard]] const char *GetType() const override
    {
        return type_.c_str();
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        return Version;
    }

    [[nodiscard]] std::string GetSourceName() const override
    {
        return source_name_;
    }

    [[nodiscard]] uint32_t GetSourceHash() const override
    {
        return source_hash_;
    }

    [[nodiscard]] CookJobResult Execute() override;

private:
    std::string type_;

    std::string source_name_;

    std::vector<char> master_payload_;

    TextureCompression::Family family_;

    uint32_t source_hash_ = 0;
};
} // namespace sparkle
