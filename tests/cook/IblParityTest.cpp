#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"
#include "core/math/Types.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
#include "io/TextureCompression.h"
#include "renderer/resource/IblBrdfCookJob.h"
#include "renderer/resource/IblEnvCookJobs.h"
#include "scene/Scene.h"
#include "scene/component/light/SkyLight.h"

#include <filesystem>

namespace sparkle
{
// gates the CPU cook jobs against their GPU producers: both must emit the same artifact.
class IblParityTest : public TestCase
{
protected:
    void OnEnforceConfigs() override
    {
        EnforceConfig("pipeline", std::string("forward"));

        // start cold so the artifacts under test come from this binary's GPU cook; the
        // family transcodes must go too or the runtime lookup would resolve them first
        auto *file_manager = FileManager::GetNativeFileManager();
        for (const char *type : {"ibl_brdf", "ibl_diffuse", "ibl_specular", "ibl_diffuse_astc", "ibl_diffuse_bc",
                                 "ibl_specular_astc", "ibl_specular_bc"})
        {
            std::filesystem::remove_all(file_manager->ResolvePath(Path::Internal(std::string("cooked/") + type)));
        }
    }

    Result OnTick(AppFramework &app) override
    {
        if (cases_.empty())
        {
            auto *sky_light = app.GetScene()->GetSkyLight();
            if ((sky_light == nullptr) || !sky_light->GetCubeMap())
            {
                return Result::Pending;
            }

            const auto &env_map = sky_light->GetCubeMap();
            const auto transcode_format = TextureCompression::SelectHdrFormat(TextureCompression::PlatformFamily);
            if (env_map->GetFormat() != PixelFormat::RGBAFloat16 && env_map->GetFormat() != transcode_format)
            {
                Log(Error, "sky cube format is {}, expected the fp16 master or the family transcode",
                    Enum2Str(env_map->GetFormat()));
                return Result::Fail;
            }

            auto brdf_job = std::make_unique<IblBrdfCookJob>();
            auto diffuse_job = std::make_unique<IblDiffuseCookJob>(env_map);
            auto specular_job = std::make_unique<IblSpecularCookJob>(env_map);

            // values are in [0, 1] where one half ULP is ~1e-3; the implementations differ
            // only in float accumulation order. the env-based maps additionally diverge at
            // cube face borders (hardware seamless filtering vs face-local bilinear) and
            // hold values up to the environment clamp, so their bounds are looser.
            cases_.push_back({.label = "ibl_brdf",
                              .key = MakeCookArtifactKey(*brdf_job),
                              .job = std::move(brdf_job),
                              .max_abs_threshold = 4e-3f,
                              .mean_abs_threshold = 1e-4f,
                              .gpu_payload = {},
                              .cpu_payload = {}});
            cases_.push_back({.label = "ibl_diffuse",
                              .key = MakeCookArtifactKey(*diffuse_job),
                              .job = std::move(diffuse_job),
                              .max_abs_threshold = 1.5e-1f,
                              .mean_abs_threshold = 5e-4f,
                              .gpu_payload = {},
                              .cpu_payload = {}});
            cases_.push_back({.label = "ibl_specular",
                              .key = MakeCookArtifactKey(*specular_job),
                              .job = std::move(specular_job),
                              .max_abs_threshold = 1.5e-1f,
                              .mean_abs_threshold = 5e-4f,
                              .gpu_payload = {},
                              .cpu_payload = {}});
            return Result::Pending;
        }

        if (!AllGpuArtifactsLoaded())
        {
            return Result::Pending;
        }

        if (!cpu_task_)
        {
            cpu_task_ = TaskManager::RunInDedicatedThread([this]() {
                for (auto &parity_case : cases_)
                {
                    auto result = parity_case.job->Execute();
                    if (result.IsSuccess())
                    {
                        parity_case.cpu_payload = result.TakePayload();
                    }
                }
            });
            return Result::Pending;
        }

        if (!cpu_task_->IsReady())
        {
            return Result::Pending;
        }

        bool all_passed = true;
        for (const auto &parity_case : cases_)
        {
            all_passed &= Compare(parity_case);
        }
        return all_passed ? Result::Pass : Result::Fail;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        // headless frames are fast (~1k fps) and the CPU cooks take minutes of wall time
        return 500000;
    }

private:
    struct ParityCase
    {
        const char *label;
        CookArtifactKey key;
        std::unique_ptr<CookJob> job;
        float max_abs_threshold;
        float mean_abs_threshold;
        std::vector<char> gpu_payload;
        std::vector<char> cpu_payload;
    };

    bool AllGpuArtifactsLoaded()
    {
        bool all_loaded = true;
        for (auto &parity_case : cases_)
        {
            if (parity_case.gpu_payload.empty())
            {
                parity_case.gpu_payload = CookArtifactStore::Load(parity_case.key);
                all_loaded &= !parity_case.gpu_payload.empty();
            }
        }
        return all_loaded;
    }

    [[nodiscard]] static bool Compare(const ParityCase &parity_case)
    {
        constexpr size_t HeaderSize = sizeof(TextureCompression::PayloadHeader);
        if (parity_case.cpu_payload.size() != parity_case.gpu_payload.size() ||
            parity_case.cpu_payload.size() <= HeaderSize)
        {
            Log(Error, "{} payload size mismatch. cpu {}. gpu {}.", parity_case.label, parity_case.cpu_payload.size(),
                parity_case.gpu_payload.size());
            return false;
        }

        // both producers emit header-wrapped fp16 masters; compare the pixels behind the header
        const auto *cpu = reinterpret_cast<const Half *>(parity_case.cpu_payload.data() + HeaderSize);
        const auto *gpu = reinterpret_cast<const Half *>(parity_case.gpu_payload.data() + HeaderSize);
        const size_t total = (parity_case.cpu_payload.size() - HeaderSize) / sizeof(Half);

        double abs_sum = 0.0;
        float abs_max = 0.f;
        size_t mismatched = 0;
        size_t count = 0;

        for (size_t i = 0; i < total; i++)
        {
            // the alpha channel is producer-dependent (the Metal blit conversion writes 1,
            // the compute shader accumulates the sample count) and no consumer reads it
            if (i % 4 == 3)
            {
                continue;
            }

            const float diff = std::abs(static_cast<float>(cpu[i]) - static_cast<float>(gpu[i]));
            abs_sum += diff;
            abs_max = std::max(abs_max, diff);
            mismatched += diff > 0.f ? 1 : 0;
            count++;
        }

        const auto abs_mean = static_cast<float>(abs_sum / static_cast<double>(count));

        const bool passed = abs_max <= parity_case.max_abs_threshold && abs_mean <= parity_case.mean_abs_threshold;

        Log(Info, "{} parity: {} values, mismatched {} ({:.3f}%), mean abs {:.6f}, max abs {:.6f} -> {}",
            parity_case.label, count, mismatched, 100.f * static_cast<float>(mismatched) / static_cast<float>(count),
            abs_mean, abs_max, passed ? "PASS" : "FAIL");

        if (!passed)
        {
            auto *file_manager = FileManager::GetNativeFileManager();
            file_manager->Write(Path::Internal(std::string("cooked_debug/") + parity_case.label + "_cpu.bin"),
                                parity_case.cpu_payload);
            file_manager->Write(Path::Internal(std::string("cooked_debug/") + parity_case.label + "_gpu.bin"),
                                parity_case.gpu_payload);
        }

        return passed;
    }

    std::vector<ParityCase> cases_;
    std::shared_ptr<TaskFuture<>> cpu_task_;
};

static TestCaseRegistrar<IblParityTest> ibl_parity_registrar("ibl_parity");
} // namespace sparkle
