#include "application/TestCase.h"

#include "core/Logger.h"
#include "renderer/denoiser/Reconstruction.h"

#include <set>
#include <utility>
#include <vector>

namespace sparkle
{
// How the denoiser and upscaler settings pair into the providers the gpu path tracer tries, and the jitter sequence a
// split upscaler reconstructs from. Runs anywhere: pure logic, no RHI.
class ReconstructionPlanTest : public TestCase
{
    Result OnTick(AppFramework & /*app*/) override
    {
        bool success = VerifyPairing();
        success &= VerifyJitter();
        return success ? Result::Pass : Result::Fail;
    }

    static bool VerifyPairing()
    {
        using D = DenoiserProvider;
        using U = UpscalerProvider;

        struct Case
        {
            D denoiser;
            U upscaler;
            ReconstructionPlan expected;
            const char *description;
        };

        const std::vector<Case> cases = {
            {D::Off, U::Off, {false, D::Off, U::Off}, "off stays off"},
            {D::Auto, U::Off, {true, D::Nrd, U::Off}, "auto denoising prefers the integrated provider, then nrd"},
            {D::Nrd, U::Off, {false, D::Nrd, U::Off}, "an explicit split denoiser skips the integrated provider"},
            {D::MetalFx, U::Off, {true, D::Nrd, U::Off}, "the integrated denoiser falls back to nrd"},
            {D::Auto, U::Auto, {true, D::Nrd, U::Fsr}, "auto on both sides prefers the integrated provider"},
            {D::Auto, U::Fsr, {false, D::Nrd, U::Fsr}, "an explicit split upscaler pairs with a split denoiser"},
            {D::Nrd, U::Auto, {false, D::Nrd, U::Fsr}, "an explicit split denoiser pairs with a split upscaler"},
            {D::Nrd, U::MetalFx, {true, D::Nrd, U::Fsr}, "an integrated upscaler forces the integrated denoiser"},
            {D::MetalFx, U::Fsr, {true, D::Nrd, U::Fsr}, "an integrated denoiser forces the integrated upscaler"},
            {D::Off, U::Auto, {false, D::Off, U::Fsr}, "upscaling without denoising uses the split upscaler"},
            {D::Off, U::MetalFx, {true, D::Off, U::Fsr}, "the integrated upscaler also denoises"},
        };

        bool success = true;
        for (const auto &[denoiser, upscaler, expected, description] : cases)
        {
            success &= Expect(PlanReconstruction(denoiser, upscaler) == expected, description);
        }
        return success;
    }

    static bool VerifyJitter()
    {
        const Vector2UInt input{640, 360};
        const Vector2UInt output{1280, 720};
        // 8 phases per output pixel per input pixel
        constexpr uint32_t PhaseCount = 32;

        bool in_range = true;
        std::set<std::pair<float, float>> phases;
        for (uint32_t index = 0; index < PhaseCount; index++)
        {
            const Vector2 jitter = ComputeJitter(index, input, output);
            in_range &= jitter.cwiseAbs().maxCoeff() <= 0.5f;
            phases.emplace(jitter.x(), jitter.y());
        }

        bool success = Expect(in_range, "jitter stays within half a pixel");
        success &= Expect(phases.size() == PhaseCount, "every phase of a period is distinct");
        success &= Expect(ComputeJitter(PhaseCount, input, output) == ComputeJitter(0, input, output),
                          "the sequence repeats after its period");
        return success;
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "ReconstructionPlanTest: OK - {}", description);
        }
        else
        {
            Log(Error, "ReconstructionPlanTest: FAILED - {}", description);
        }
        return condition;
    }
};

static TestCaseRegistrar<ReconstructionPlanTest> reconstruction_plan_test_registrar("reconstruction_plan");
} // namespace sparkle
