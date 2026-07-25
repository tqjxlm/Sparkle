#include "application/CookPipeline.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/Path.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"
#include "io/CookTargets.h"
#include "io/HdrCubeTranscodeJob.h"
#include "io/TextureCookJob.h"
#include "renderer/RenderConfig.h"
#include "renderer/resource/IblCookAccelerator.h"
#include "renderer/resource/IblCookPlan.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/PrimitiveComponent.h"
#include "scene/cook/SceneCooker.h"
#include "scene/material/Material.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace sparkle
{
namespace
{
std::string JobManifestKey(const CookJob &job)
{
    return CookArtifactStore::GetManifestKey(MakeCookIdentityKey(job));
}

using ConsumedSourceMap = std::map<std::string, std::map<TextureCompression::Family, std::set<std::string>>>;
using FamilyArtifactMap = std::map<TextureCompression::Family, std::set<std::string>>;

struct PendingHdrTranscode
{
    std::string master_type;
    std::string source_name;
    CookArtifactKey master_key;
    // the sky map source hash for the sky entry; zero for the IBL entries, whose
    // transcode hash chains from the family sky cube cooked before them
    uint32_t origin_hash = 0;
};

// derives the per-family HDR cube artifacts from the fp16 masters the run produced. block
// encoding dominates a release cook, so the transcodes run as concurrent cook jobs instead
// of one at a time: the families are independent, while an IBL entry chains from the encoded
// bytes of the sky cube of its own family and therefore waits for it. deliveries run on the
// pumping thread, so the callbacks below need no synchronization
bool CookHdrTranscodes(const std::vector<PendingHdrTranscode> &pending,
                       const std::set<TextureCompression::Family> &families, FamilyArtifactMap &family_artifacts)
{
    // a transcode holds its fp16 master until it finishes, so cap the concurrent ones
    constexpr size_t MaxInFlightTranscodes = 4;

    bool failed = false;
    std::vector<CookHandle> handles;
    std::map<TextureCompression::Family, uint32_t> family_sky_cube_hashes;

    using PayloadHandler = std::function<bool(const CookPayload &)>;

    auto request = [&](TextureCompression::Family family, const PendingHdrTranscode &entry, uint32_t origin_hash,
                       PayloadHandler on_payload) {
        auto master = CookArtifactStore::Load(entry.master_key);
        if (master.empty())
        {
            Log(Error, "missing master artifact for {}", entry.source_name);
            failed = true;
            return;
        }

        ASSERT_F(origin_hash != 0, "sky transcode must precede the IBL transcodes");
        const auto source_hash = HdrCubeTranscodeJob::MakeSourceHash(origin_hash, entry.master_key.version);
        auto job = std::make_shared<HdrCubeTranscodeJob>(entry.master_type, family, entry.source_name,
                                                         std::move(master), source_hash);
        const auto key = MakeCookArtifactKey(*job);

        handles.push_back(Cooker::Request(
            key, [job]() { return job; },
            [&failed, &family_artifacts, key, family, source_name = entry.source_name,
             handle_payload = std::move(on_payload)](CookResult result) {
                if (!result.HasPayload() || (handle_payload && !handle_payload(result.payload)))
                {
                    Log(Error, "failed to transcode {} for {}", source_name, TextureCompression::GetFamilyName(family));
                    failed = true;
                    return;
                }
                family_artifacts[family].insert(CookArtifactStore::GetManifestKey(key));
            }));

        SceneCooker::PumpMainThreadUntil([&handles] {
            std::erase_if(handles, [](const CookHandle &handle) { return handle.OnDelivered()->IsReady(); });
            return handles.size() < MaxInFlightTranscodes;
        });
    };

    auto wait_for_all = [&handles]() {
        SceneCooker::PumpMainThreadUntil([&handles] {
            return std::ranges::all_of(handles,
                                       [](const CookHandle &handle) { return handle.OnDelivered()->IsReady(); });
        });
        handles.clear();
    };

    // one round per sky map: its families' sky cubes first, then every IBL entry the sky
    // entry introduced, each keyed by the sky cube its own family just produced
    for (auto entry = pending.begin(); entry != pending.end() && !failed;)
    {
        const auto &sky_entry = *entry;
        ASSERT_F(sky_entry.origin_hash != 0, "a transcode round starts at a sky entry");

        for (auto family : families)
        {
            request(family, sky_entry, sky_entry.origin_hash, [&, family](const CookPayload &payload) {
                auto family_cube = SkyLight::MakeCubeFromPayload(payload, sky_entry.source_name);
                if (!family_cube)
                {
                    return false;
                }
                family_sky_cube_hashes[family] = family_cube->GetContentHash();
                return true;
            });
        }
        wait_for_all();

        const auto round_end =
            std::find_if(++entry, pending.end(), [](const PendingHdrTranscode &next) { return next.origin_hash != 0; });
        for (; entry != round_end && !failed; ++entry)
        {
            for (auto family : families)
            {
                request(family, *entry, family_sky_cube_hashes[family], {});
            }
        }
        wait_for_all();
    }

    return !failed;
}

void CollectMaterialTextureJobs(const Scene &scene, const std::set<TextureCompression::Family> &families,
                                std::vector<std::unique_ptr<CookJob>> &jobs, ConsumedSourceMap &consumed_sources)
{
    std::unordered_set<std::string> seen;
    for (const auto *primitive : scene.GetPrimitives())
    {
        const auto *material = primitive->GetMaterial();
        if (material == nullptr)
        {
            continue;
        }

        ForEachMaterialTexture(material->GetRawMaterial(), [&seen, &jobs, &families,
                                                            &consumed_sources](const std::shared_ptr<Image2D> &texture,
                                                                               TextureCompression::Profile profile) {
            if (!texture || !IsCookableMaterialTexture(*texture))
            {
                return;
            }

            for (auto family : families)
            {
                auto job = std::make_unique<TextureCookJob>(texture, texture->GetName(), profile, family);
                auto manifest_key = JobManifestKey(*job);
                consumed_sources[texture->GetName()][family].insert(manifest_key);
                if (seen.insert(std::move(manifest_key)).second)
                {
                    jobs.push_back(std::move(job));
                }
            }
        });
    }
}

// read by assemble_cooked_image in build.py; the cook output dir persists across runs,
// so a smaller result must still overwrite a stale file
bool WriteCookProducts(const std::vector<std::string> &targets, const std::set<std::string> &universal_keys,
                       const ConsumedSourceMap &consumed_sources, const FamilyArtifactMap &family_artifacts)
{
    nlohmann::json json = nlohmann::json::object();
    for (const auto &target : targets)
    {
        const auto family = CookTargets::FamilyOf(target);

        std::set<std::string> artifacts = universal_keys;
        if (const auto family_keys = family_artifacts.find(family); family_keys != family_artifacts.end())
        {
            artifacts.insert(family_keys->second.begin(), family_keys->second.end());
        }

        nlohmann::json consumed_json = nlohmann::json::object();
        for (const auto &[source, family_keys] : consumed_sources)
        {
            const auto keys = family_keys.find(family);
            if (keys == family_keys.end())
            {
                continue;
            }
            artifacts.insert(keys->second.begin(), keys->second.end());
            consumed_json[source] = keys->second;
        }

        json[target] = {{"artifacts", artifacts}, {"consumed_sources", consumed_json}};
    }

    const auto dump = json.dump(2);
    const auto written = FileManager::GetNativeFileManager()->Write(Path::Internal("cooked/cook_products.json"),
                                                                    dump.data(), dump.size());
    return !written.empty();
}
} // namespace

int RunCookPipeline(const std::vector<std::string> &targets, const std::string &scene_path, RHIContext *rhi,
                    const RenderConfig &render_config)
{
    SceneCooker::JobAccelerator accelerator;
    if (rhi && rhi->HasPhysicalGpu())
    {
        accelerator = [rhi, &render_config](const CookJob &job) {
            return IblCookAccelerator::TryCook(job, rhi, render_config);
        };
    }

    std::set<TextureCompression::Family> texture_families;
    for (const auto &target : targets)
    {
        texture_families.insert(CookTargets::FamilyOf(target));
        Log(Info, "cook target: {}", target);
    }

    // scene loading must keep raw material textures so the plan can cook the requested families
    SetMaterialTextureInlineResolve(false);

    ConsumedSourceMap consumed_texture_sources;
    std::set<std::string> universal_keys;
    std::vector<PendingHdrTranscode> pending_transcodes;

    const SceneCooker::JobPlan job_plan{
        .collect_scene_independent_jobs =
            [&universal_keys](std::vector<std::unique_ptr<CookJob>> &jobs) {
                IblCookPlan::CollectSceneIndependentJobs(jobs);
                for (const auto &job : jobs)
                {
                    universal_keys.insert(JobManifestKey(*job));
                }
                return true;
            },
        .collect_scene_jobs =
            [&consumed_texture_sources, &texture_families,
             &pending_transcodes](const Scene &scene, std::vector<std::unique_ptr<CookJob>> &jobs) {
                const auto *sky_light = scene.GetSkyLight();
                CollectMaterialTextureJobs(scene, texture_families, jobs, consumed_texture_sources);

                if (sky_light == nullptr || !sky_light->HasSkyMap())
                {
                    return true;
                }

                // the fp16 sky and IBL masters cook once; the per-family transcodes derive
                // from them after the run so mixed-family target sets ship the format each
                // target can sample
                const auto sky_map_path = sky_light->GetSkyMapPath();
                auto master = SkyLight::CookMasterPayload(sky_map_path);
                auto cube = master.HasPayload() && master.source_hash
                                ? SkyLight::MakeCubeFromPayload(master.payload, sky_map_path)
                                : nullptr;
                if (!cube)
                {
                    Log(Error, "failed to cook sky cube {}", sky_map_path);
                    return false;
                }

                const auto sky_key = SkyLight::MasterCookKey(sky_map_path);
                pending_transcodes.push_back({sky_key.type, sky_key.source_name, sky_key, *master.source_hash});

                std::vector<std::unique_ptr<CookJob>> env_jobs;
                IblCookPlan::CollectEnvironmentJobs(cube, env_jobs);
                for (auto &job : env_jobs)
                {
                    pending_transcodes.push_back({job->GetType(), job->GetSourceName(), MakeCookArtifactKey(*job), 0});
                    jobs.push_back(std::move(job));
                }
                return true;
            }};

    auto exit_code = SceneCooker::Run(scene_path, job_plan, accelerator);

    FamilyArtifactMap hdr_family_artifacts;
    if (exit_code == 0 && !CookHdrTranscodes(pending_transcodes, texture_families, hdr_family_artifacts))
    {
        exit_code = 1;
    }

    if (exit_code == 0 && !WriteCookProducts(targets, universal_keys, consumed_texture_sources, hdr_family_artifacts))
    {
        Log(Error, "failed to write the cook products manifest");
        exit_code = 1;
    }

    return exit_code;
}
} // namespace sparkle
