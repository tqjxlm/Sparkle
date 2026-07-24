#include "renderer/resource/IblCookAccelerator.h"

#include "core/Exception.h"
#include "core/cook/CookJob.h"
#include "io/TextureCompression.h"
#include "renderer/pass/IBLBrdfPass.h"
#include "renderer/pass/IBLDiffusePass.h"
#include "renderer/pass/IBLSpecularPass.h"
#include "renderer/resource/IblBrdfCookJob.h"
#include "renderer/resource/IblEnvCookJobs.h"
#include "rhi/RHI.h"

#include <cmath>
#include <cstring>

namespace sparkle
{
namespace
{
// a broken readback (e.g. a partially written blit destination) must fail the cook
// loudly instead of persisting garbage texels as an artifact
bool PayloadHasNan(const std::vector<char> &payload)
{
    constexpr size_t HeaderSize = sizeof(TextureCompression::PayloadHeader);
    if (payload.size() <= HeaderSize)
    {
        return false;
    }

    TextureCompression::PayloadHeader header;
    std::memcpy(&header, payload.data(), sizeof(header));
    if (static_cast<PixelFormat>(header.format) != PixelFormat::RGBAFloat16)
    {
        return false;
    }

    const auto *values = reinterpret_cast<const Half *>(payload.data() + HeaderSize);
    const size_t count = (payload.size() - HeaderSize) / sizeof(Half);
    for (size_t i = 0; i < count; i++)
    {
        if (std::isnan(static_cast<float>(values[i])))
        {
            return true;
        }
    }
    return false;
}

CookJobResult DrivePassToCompletion(RHIContext *rhi, std::unique_ptr<IBLPass> pass, const RenderConfig &config)
{
    std::vector<char> payload;
    pass->SetArtifactReadyCallback([&payload](std::vector<char> result) { payload = std::move(result); });
    pass->InitRenderResources(config);

    constexpr unsigned SamplesPerDispatch = 64;
    while (!pass->IsReady())
    {
        if (!rhi->BeginFrame())
        {
            return CookJobResult::Failure();
        }
        pass->CookOnTheFly(config, SamplesPerDispatch);
        rhi->EndFrame();
    }
    rhi->WaitForDeviceIdle();

    if (payload.empty())
    {
        return CookJobResult::Failure();
    }
    if (PayloadHasNan(payload))
    {
        Log(Error, "GPU cook read back NaN texels; rejecting the artifact");
        return CookJobResult::Failure();
    }
    return CookJobResult::Success(std::move(payload));
}

// cook jobs run outside any frame, so the upload needs an explicit command buffer scope
RHIResourceRef<RHIImage> CreateEnvironmentMap(RHIContext *rhi, const Image2DCube *env_map, const std::string &name)
{
    rhi->BeginCommandBuffer();
    auto env_rhi = rhi->CreateTextureCube(env_map, name);
    rhi->SubmitCommandBuffer();
    return env_rhi;
}
} // namespace

CookJobResult IblCookAccelerator::TryCook(const CookJob &job, RHIContext *rhi, const RenderConfig &config)
{
    ASSERT(rhi);

    if (dynamic_cast<const IblBrdfCookJob *>(&job) != nullptr)
    {
        return DrivePassToCompletion(rhi, std::make_unique<IBLBrdfPass>(rhi), config);
    }

    if (const auto *diffuse = dynamic_cast<const IblDiffuseCookJob *>(&job))
    {
        auto env_rhi = CreateEnvironmentMap(rhi, diffuse->env_map_.get(), diffuse->GetSourceName());
        return DrivePassToCompletion(rhi, std::make_unique<IBLDiffusePass>(rhi, env_rhi), config);
    }

    if (const auto *specular = dynamic_cast<const IblSpecularCookJob *>(&job))
    {
        auto env_rhi = CreateEnvironmentMap(rhi, specular->env_map_.get(), specular->GetSourceName());
        return DrivePassToCompletion(rhi, std::make_unique<IBLSpecularPass>(rhi, env_rhi), config);
    }

    return CookJobResult::Unsupported();
}
} // namespace sparkle
