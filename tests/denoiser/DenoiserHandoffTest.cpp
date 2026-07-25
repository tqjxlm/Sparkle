#include "application/TestCase.h"

#include "core/Logger.h"
#include "renderer/denoiser/DenoiserHandoff.h"

namespace sparkle
{
// The convergence handoff schedule every denoising provider shares. Providers are compared
// against each other by the denoiser gates, so the schedule's shape is a contract: the window
// bounds, the linear ramp between them, the pin to the accumulator on the final frame, and the
// unconditional opt-out below the window. Runs anywhere: pure arithmetic, no RHI.
//
// Expect logs on success as well as failure, so a swept property is reduced to one assertion
// rather than one per sample: on android every line also crosses logcat, and a few hundred of
// them cost minutes of capture even though the arithmetic itself takes milliseconds.
class DenoiserHandoffTest : public TestCase
{
    Result OnTick(AppFramework & /*app*/) override
    {
        bool success = VerifyOptOutBelowWindow();
        success &= VerifyRampInsideWindow();
        success &= VerifyFinalFramePinsToAccumulator();
        success &= VerifyWindowShrinksWithMaxSamples();

        return success ? Result::Pass : Result::Fail;
    }

    // a max_spp below the window never blends the accumulator in, whatever the accumulator
    // reached: a frame's spp is not bounded by the distance left to max_spp, so an opted-out
    // render can still push the count past the window's start
    static bool VerifyOptOutBelowWindow()
    {
        bool opts_out = true;
        bool stays_zero = true;
        for (const uint32_t max_spp : {1u, 2u, 100u, 511u})
        {
            const DenoiserHandoff handoff(max_spp);
            opts_out &= !handoff.Applies();
            for (const float samples : {0.f, 100.f, 600.f, 3000.f})
            {
                stays_zero &= ExactlyEquals(handoff.ComputeWeight(samples, false), 0.f);
                stays_zero &= ExactlyEquals(handoff.ComputeWeight(samples, true), 0.f);
            }
        }

        bool success = Expect(opts_out, "every max_spp below the window opts out");
        success &= Expect(stays_zero, "an opted-out handoff never blends the accumulator in");
        return success;
    }

    static bool VerifyRampInsideWindow()
    {
        const DenoiserHandoff handoff(4096);
        bool success = Expect(handoff.Applies(), "applies at max_spp above the window");
        success &= Expect(ExactlyEquals(handoff.GetEnd(), DenoiserHandoff::EndSamples), "end is clamped to the window");
        success &= Expect(ExactlyEquals(handoff.ComputeWeight(DenoiserHandoff::StartSamples, false), 0.f),
                          "starts on the denoiser");
        success &= Expect(ExactlyEquals(handoff.ComputeWeight(DenoiserHandoff::StartSamples - 1.f, false), 0.f),
                          "clamps below the window start");
        success &= Expect(ExactlyEquals(handoff.ComputeWeight(DenoiserHandoff::EndSamples, false), 1.f),
                          "ends on the accumulator");
        success &= Expect(ExactlyEquals(handoff.ComputeWeight(DenoiserHandoff::EndSamples + 1000.f, false), 1.f),
                          "clamps above the window end");

        const float midpoint = 0.5f * (DenoiserHandoff::StartSamples + DenoiserHandoff::EndSamples);
        success &=
            Expect(ExactlyEquals(handoff.ComputeWeight(midpoint, false), 0.5f), "ramps linearly across the window");

        // monotonic: a converging accumulator never loses ground to the denoiser
        float previous = -1.f;
        bool monotonic = true;
        for (uint32_t step = 0; step <= 3000 / 7; step++)
        {
            const float weight = handoff.ComputeWeight(static_cast<float>(step * 7), false);
            monotonic &= weight >= previous;
            previous = weight;
        }
        success &= Expect(monotonic, "weight never decreases as samples accumulate");
        return success;
    }

    static bool VerifyFinalFramePinsToAccumulator()
    {
        const DenoiserHandoff handoff(1024);
        bool success = Expect(handoff.ComputeWeight(600.f, false) < 1.f, "mid-window frame still blends");
        // the frozen frame must equal the accumulator bit-exactly
        success &=
            Expect(ExactlyEquals(handoff.ComputeWeight(600.f, true), 1.f), "final frame pins to the accumulator");
        return success;
    }

    // max_spp inside the window shortens it, so a render that stops early still reaches 1
    static bool VerifyWindowShrinksWithMaxSamples()
    {
        const DenoiserHandoff handoff(1024);
        bool success = Expect(handoff.Applies(), "applies at max_spp inside the window");
        success &= Expect(ExactlyEquals(handoff.GetEnd(), 1024.f), "end follows max_spp inside the window");
        success &=
            Expect(ExactlyEquals(handoff.ComputeWeight(1024.f, false), 1.f), "reaches the accumulator at max_spp");
        return success;
    }

    // the handoff contract is exact, not approximate: the weight is precisely 0 and 1 at the
    // boundaries so a frozen frame equals the accumulator bit-for-bit. clang-cl enables
    // -Wfloat-equal through -Weverything, so the deliberate exactness is spelled out here rather
    // than at each assertion
    [[nodiscard]] static bool ExactlyEquals(float value, float expected)
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
        return value == expected;
#pragma clang diagnostic pop
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "DenoiserHandoffTest: OK - {}", description);
        }
        else
        {
            Log(Error, "DenoiserHandoffTest: FAILED - {}", description);
        }
        return condition;
    }
};

static TestCaseRegistrar<DenoiserHandoffTest> denoiser_handoff_test_registrar("denoiser_handoff");
} // namespace sparkle
