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
        Log(Info, "{}: NRD v{}.{}.{} spirvOffsets s={} b={} u={} t={}", GetName(), lib.versionMajor,
            lib.versionMinor, lib.versionBuild, lib.spirvBindingOffsets.samplerOffset,
            lib.spirvBindingOffsets.constantBufferOffset, lib.spirvBindingOffsets.storageTextureAndBufferOffset,
            lib.spirvBindingOffsets.textureOffset);

        NrdCookedShaders cooked;
        if (!LoadNrdCookedShaders(cooked))
        {
            Log(Error, "{}: failed to load cooked shaders", GetName());
            return Result::Fail;
        }
        Log(Info, "{}: cooked shaders v{}.{}.{}, {} pipelines", GetName(), cooked.version_major,
            cooked.version_minor, cooked.version_build, cooked.pipelines.size());

        MetalNrdBackend backend(device);
        for (uint32_t i = 0; i < cooked.pipelines.size(); i++)
        {
            if (!backend.AddPipeline(cooked.pipelines[i]))
            {
                Log(Error, "{}: [{:2}] {} failed", GetName(), i, cooked.identifiers[i]);
            }
        }
        const uint32_t created = backend.GetPipelineCount();

        Log(Info, "{}: pipelines created {}/{}", GetName(), created, cooked.pipelines.size());
        return created == cooked.pipelines.size() ? Result::Pass : Result::Fail;
    }
};

static TestCaseRegistrar<NrdProbeTest> nrd_probe_registrar("nrd_probe");
} // namespace sparkle

#endif
