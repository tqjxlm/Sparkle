#include "io/TextureCookJob.h"

#include "core/Logger.h"
#include "core/cook/Cooker.h"

#include <atomic>

namespace sparkle
{
namespace
{
std::atomic<bool> inline_resolve_enabled{true};

const char *GetProfileName(TextureCompression::Profile profile)
{
    switch (profile)
    {
    case TextureCompression::Profile::Color:
        return "color";
    case TextureCompression::Profile::Data:
        return "data";
    case TextureCompression::Profile::Normal:
        return "normal";
    default:
        UnImplemented(profile);
        return "";
    }
}
} // namespace

const char *TextureCookJob::GetTypeName(TextureCompression::Family family)
{
    return family == TextureCompression::Family::Astc ? "texture_astc" : "texture_bc";
}

std::string TextureCookJob::MakeSourceName(const std::string &identity, TextureCompression::Profile profile)
{
    return identity + "#" + GetProfileName(profile);
}

TextureCookJob::TextureCookJob(std::shared_ptr<const Image2D> source, std::string identity,
                               TextureCompression::Profile profile, TextureCompression::Family family)
    : source_(std::move(source)), identity_(std::move(identity)), profile_(profile), family_(family)
{
    ASSERT(source_ && source_->IsValid());
}

uint32_t TextureCookJob::GetSourceHash() const
{
    return source_->GetContentHash();
}

CookJobResult TextureCookJob::Execute()
{
    auto payload = TextureCompression::Encode(*source_, profile_, family_);
    if (payload.empty())
    {
        return CookJobResult::Failure();
    }
    return CookJobResult::Success(std::move(payload));
}

void ForEachMaterialTexture(
    const MaterialResource &material,
    const std::function<void(const std::shared_ptr<Image2D> &, TextureCompression::Profile)> &visit)
{
    visit(material.base_color_texture, TextureCompression::Profile::Color);
    visit(material.emissive_texture, TextureCompression::Profile::Color);
    visit(material.metallic_roughness_texture, TextureCompression::Profile::Data);
    visit(material.normal_texture, TextureCompression::Profile::Normal);
}

bool IsCompressibleImagePath(const std::string &path)
{
    return path.ends_with(".png") || path.ends_with(".jpg") || path.ends_with(".jpeg");
}

bool IsCookableMaterialTexture(const Image2D &image)
{
    return (image.GetFormat() == PixelFormat::R8G8B8A8Srgb || image.GetFormat() == PixelFormat::R8G8B8A8Unorm) &&
           IsCompressibleImagePath(image.GetName());
}

std::shared_ptr<Image2D> ResolveMaterialTexture(const std::shared_ptr<Image2D> &source, const std::string &identity,
                                                TextureCompression::Profile profile)
{
    if (identity.empty() || !inline_resolve_enabled.load(std::memory_order_relaxed))
    {
        return source;
    }

    // only 8-bit sources compress; HDR material textures pass through untouched
    if (source && source->GetFormat() != PixelFormat::R8G8B8A8Srgb && source->GetFormat() != PixelFormat::R8G8B8A8Unorm)
    {
        return source;
    }

    const CookArtifactKey lookup_key{.type = TextureCookJob::GetTypeName(TextureCompression::PlatformFamily),
                                     .version = TextureCookJob::Version,
                                     .source_name = TextureCookJob::MakeSourceName(identity, profile),
                                     .source_hash = std::nullopt};

    auto result = Cooker::CookNow(lookup_key, [&source, &identity, profile]() -> std::shared_ptr<CookJob> {
        if (!source || !source->IsValid())
        {
            return nullptr;
        }
        return std::make_shared<TextureCookJob>(source, identity, profile, TextureCompression::PlatformFamily);
    });

    if (!result.HasPayload())
    {
        if (source)
        {
            return source;
        }
        Log(Error, "material texture {} has neither a cooked artifact nor a readable source", identity);
        return nullptr;
    }

    auto compressed = TextureCompression::CreateImageFromPayload(result.payload, identity);
    return compressed ? compressed : source;
}

void SetMaterialTextureInlineResolve(bool enabled)
{
    inline_resolve_enabled.store(enabled, std::memory_order_relaxed);
}
} // namespace sparkle
