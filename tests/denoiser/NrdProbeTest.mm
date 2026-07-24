#if FRAMEWORK_APPLE

#include "application/TestCase.h"

#include "core/Logger.h"
#include "renderer/nrd/NrdCookedShaders.h"

#include "../../libraries/source/rhi/metal/MetalNrdBackend.h"

#include "NRD.h"

#import <Metal/Metal.h>

namespace sparkle
{
// PSO creation (not just MSL text) is the real gate: it proves this GPU supports the SIMD/quad-group ops
// ReBLUR uses. Drives the production cooked-shader loader + MetalNrdBackend::AddPipeline so the probe
// cannot drift from the path the engine ships. A fresh default device is the same physical GPU, so no
// RHI coupling.
// Usage: --test_case nrd_probe
class NrdProbeTest : public TestCase
{
public:
    Result OnTick(AppFramework &) override
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            Log(Error, "{}: MTLCreateSystemDefaultDevice returned nil", GetName());
            return Result::Fail;
        }

        const nrd::LibraryDesc &lib = *nrd::GetLibraryDesc();
        Log(Info, "{}: NRD v{}.{}.{} spirvOffsets s={} b={} u={} t={}", GetName(), lib.versionMajor, lib.versionMinor,
            lib.versionBuild, lib.spirvBindingOffsets.samplerOffset, lib.spirvBindingOffsets.constantBufferOffset,
            lib.spirvBindingOffsets.storageTextureAndBufferOffset, lib.spirvBindingOffsets.textureOffset);

        NrdCookedShaders cooked;
        if (!cooked.Load())
        {
            Log(Error, "{}: failed to load cooked shaders", GetName());
            return Result::Fail;
        }
        Log(Info, "{}: cooked shaders v{}.{}.{}", GetName(), cooked.VersionMajor(), cooked.VersionMinor(),
            cooked.VersionBuild());

        nrd::DenoiserDesc denoiser{};
        denoiser.identifier = 0;
        denoiser.denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        nrd::InstanceCreationDesc creation{};
        creation.denoisers = &denoiser;
        creation.denoisersNum = 1;
        nrd::Instance *instance = nullptr;
        if (nrd::CreateInstance(creation, instance) != nrd::Result::SUCCESS)
        {
            Log(Error, "{}: nrd::CreateInstance failed", GetName());
            return Result::Fail;
        }
        const nrd::InstanceDesc &desc = *nrd::GetInstanceDesc(*instance);

        MetalNrdBackend backend(device);
        for (uint32_t i = 0; i < desc.pipelinesNum; i++)
        {
            RHINrdBackend::CookedPipeline pipeline;
            if (!cooked.BuildPipeline(desc.pipelines[i].shaderIdentifier, pipeline) || !backend.AddPipeline(pipeline))
            {
                Log(Error, "{}: [{:2}] {} failed", GetName(), i, desc.pipelines[i].shaderIdentifier);
            }
        }
        const uint32_t created = backend.GetPipelineCount();

        nrd::DestroyInstance(*instance);

        Log(Info, "{}: pipelines created {}/{}", GetName(), created, desc.pipelinesNum);
        return created == desc.pipelinesNum ? Result::Pass : Result::Fail;
    }
};

static TestCaseRegistrar<NrdProbeTest> nrd_probe_registrar("nrd_probe");
} // namespace sparkle

#endif
