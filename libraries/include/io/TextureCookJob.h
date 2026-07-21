#pragma once

#include "core/cook/CookJob.h"
#include "io/Material.h"
#include "io/TextureCompression.h"

#include <functional>

namespace sparkle
{
// compresses one material texture into a block-compressed artifact. one artifact per
// (source image, profile, family); the family lives in the artifact type so packaging
// can filter per platform
class TextureCookJob : public CookJob
{
public:
    static constexpr uint32_t Version = 1;

    [[nodiscard]] static const char *GetTypeName(TextureCompression::Family family);

    [[nodiscard]] static std::string MakeSourceName(const std::string &identity, TextureCompression::Profile profile);

    TextureCookJob(std::shared_ptr<const Image2D> source, std::string identity, TextureCompression::Profile profile,
                   TextureCompression::Family family);

    [[nodiscard]] const char *GetType() const override
    {
        return GetTypeName(family_);
    }

    [[nodiscard]] uint32_t GetVersion() const override
    {
        return Version;
    }

    [[nodiscard]] std::string GetSourceName() const override
    {
        return MakeSourceName(identity_, profile_);
    }

    [[nodiscard]] uint32_t GetSourceHash() const override;

    [[nodiscard]] CookJobResult Execute() override;

private:
    std::shared_ptr<const Image2D> source_;
    std::string identity_;
    TextureCompression::Profile profile_;
    TextureCompression::Family family_;
};

// the four material texture slots and their compression profiles in one place
void ForEachMaterialTexture(
    const MaterialResource &material,
    const std::function<void(const std::shared_ptr<Image2D> &, TextureCompression::Profile)> &visit);

[[nodiscard]] bool IsCompressibleImagePath(const std::string &path);

// a cookable material texture is an 8-bit image whose name is its packed-relative
// source path (set by the scene loaders)
[[nodiscard]] bool IsCookableMaterialTexture(const Image2D &image);

// resolves a loader-produced material texture to its cooked block-compressed form for
// the platform family: packaged/cached artifact hit, or an inline cook when source
// pixels exist. identity is the packed-relative asset path; without one (procedural or
// embedded-only content) the source passes through unchanged. a null source resolves
// from the manifest alone, which is how stripped packages load textures
[[nodiscard]] std::shared_ptr<Image2D> ResolveMaterialTexture(const std::shared_ptr<Image2D> &source,
                                                              const std::string &identity,
                                                              TextureCompression::Profile profile);

// build-time cook mode loads scenes only to enumerate cook inputs; it disables inline
// resolution so material sources keep raw pixels for the job plan
void SetMaterialTextureInlineResolve(bool enabled);
} // namespace sparkle
