#include "scene/component/light/SkyLight.h"

#include <crc32.h>

#include <atomic>
#include <optional>

#include "core/Logger.h"
#include "core/cook/Cooker.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "io/HdrCubeTranscodeJob.h"
#include "io/Image.h"
#include "io/TextureCompression.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/IblSettings.h"
#include "scene/Scene.h"
#include <filesystem>

namespace sparkle
{
constexpr unsigned CubeMapSize = 1024;
constexpr Scalar MaxSkyBrightness = 100.f;

namespace
{
class SkyLightCookJob : public CookJob
{
public:
    static constexpr const char *Type = "skylight";
    static constexpr uint32_t Version = 3;

    SkyLightCookJob(std::shared_ptr<const Image2D> sky_map, std::string source_name)
        : sky_map_(std::move(sky_map)), source_name_(std::move(source_name))
    {
        CRC32 hasher;
        hasher.add(sky_map_->GetRawData(), sky_map_->GetStorageSize());
        const std::array<uint32_t, 3> meta{sky_map_->GetWidth(), sky_map_->GetHeight(),
                                           static_cast<uint32_t>(sky_map_->GetFormat())};
        hasher.add(meta.data(), meta.size() * sizeof(uint32_t));
        hasher.getHash(reinterpret_cast<unsigned char *>(&source_hash_));
    }

    [[nodiscard]] const char *GetType() const override
    {
        return Type;
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

    [[nodiscard]] float GetProgress() const override
    {
        return static_cast<float>(cooked_row_count_.load()) / (CubeMapSize * Image2DCube::FaceId::Count);
    }

    [[nodiscard]] CookJobResult Execute() override;

private:
    std::shared_ptr<const Image2D> sky_map_;

    std::string source_name_;

    uint32_t source_hash_ = 0;

    std::atomic<uint32_t> cooked_row_count_ = 0;
};

CookJobResult SkyLightCookJob::Execute()
{
    Image2DCube cube_map(CubeMapSize, CubeMapSize, PixelFormat::RGBAFloat16, sky_map_->GetName() + "_CubeMap");

    std::array<Scalar, Image2DCube::FaceId::Count> max_brightness_per_face;
    std::array<Vector3, Image2DCube::FaceId::Count> max_brightness_dir_per_face;
    std::array<Vector3, Image2DCube::FaceId::Count> subtracted_color_per_face;

    std::ranges::fill(max_brightness_per_face, 0.f);
    std::ranges::fill(max_brightness_dir_per_face, Zeros);
    std::ranges::fill(subtracted_color_per_face, Zeros);

    std::vector<std::shared_ptr<TaskFuture<void>>> cube_map_tasks(Image2DCube::FaceId::Count);

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        auto face_id = static_cast<Image2DCube::FaceId>(id);
        auto &this_face = cube_map.GetFace(face_id);

        cube_map_tasks[id] = TaskManager::RunInWorkerThread([this, face_id, &this_face, &max_brightness_per_face,
                                                             &max_brightness_dir_per_face,
                                                             &subtracted_color_per_face]() {
            for (unsigned i = 0; i < CubeMapSize; i++)
            {
                for (unsigned j = 0; j < CubeMapSize; j++)
                {
                    auto u = (static_cast<Scalar>(i) + 0.5f) / static_cast<Scalar>(CubeMapSize) * 2.f - 1.f;
                    auto v = (static_cast<Scalar>(j) + 0.5f) / static_cast<Scalar>(CubeMapSize) * 2.f - 1.f;

                    Vector3 direction = Image2DCube::TextureCoordinateToDirection(face_id, u, v);

                    Vector2 eq_uv = utilities::CartesianToEquirectangular(direction);
                    Vector3 color = sky_map_->Sample(eq_uv);

                    this_face.SetPixel(i, j, color);

                    auto brightness = color.norm();

                    if (brightness > max_brightness_per_face[face_id])
                    {
                        max_brightness_per_face[face_id] = brightness;
                        max_brightness_dir_per_face[face_id] = direction;
                    }

                    // in order to support explicit directional lighting for rasterization-based rendering,
                    // we subtract some color when cooking IBL and use it as the directional light.
                    if (brightness > IblSettings::MaxEnvironmentBrightness)
                    {
                        // TODO(tqjxlm): MaxBrightness is not analytically correct.
                        Vector3 subtracted_color = utilities::ClampLength(color, MaxSkyBrightness) -
                                                   utilities::ClampLength(color, IblSettings::MaxEnvironmentBrightness);

                        constexpr float Numerator = 4.f * (1.f / CubeMapSize) * (1.f / CubeMapSize);
                        float solid_angle = Numerator / std::pow(1 + u * u + v * v, 1.5f);

                        subtracted_color_per_face[face_id] += subtracted_color * solid_angle;
                    }
                }

                cooked_row_count_++;
            }
        });
    }

    TaskManager::OnAll(cube_map_tasks)->Wait();

    Scalar max_brightness = 0.f;
    Vector3 max_brightness_dir;
    Vector3 sun_brightness = Zeros;
    for (unsigned i = 0; i < Image2DCube::FaceId::Count; i++)
    {
        if (max_brightness_per_face[i] > max_brightness)
        {
            max_brightness = max_brightness_per_face[i];
            max_brightness_dir = max_brightness_dir_per_face[i];
        }

        sun_brightness += subtracted_color_per_face[i];
    }

    Eigen::Quaternion<Scalar> q = Eigen::Quaternion<Scalar>::FromTwoVectors(Front, max_brightness_dir);

    Vector3 sun_direction = q.toRotationMatrix().canonicalEulerAngles(0, 1, 2);

    Log(Info, "sky map cook ok. max brightness {}. max brightness direction {}. sun brightness {}", max_brightness,
        utilities::VectorToString(utilities::ToDegree(sun_direction)), utilities::VectorToString(sun_brightness));

    const size_t face_size = static_cast<size_t>(CubeMapSize) * CubeMapSize * GetPixelSize(PixelFormat::RGBAFloat16);

    std::vector<char> payload(sizeof(TextureCompression::PayloadHeader) + 6 * face_size + 2 * sizeof(float) * 3);
    char *ptr = payload.data();

    const TextureCompression::PayloadHeader header{.format = static_cast<uint32_t>(PixelFormat::RGBAFloat16),
                                                   .width = CubeMapSize,
                                                   .height = CubeMapSize,
                                                   .mip_count = 1};
    std::memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        std::memcpy(ptr, cube_map.GetFace(static_cast<Image2DCube::FaceId>(id)).GetRawData(), face_size);
        ptr += face_size;
    }

    std::memcpy(ptr, sun_brightness.data(), sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    std::memcpy(ptr, sun_direction.data(), sizeof(float) * 3);

    return CookJobResult::Success(std::move(payload));
}

struct SkyPayloadView
{
    PixelFormat format;
    const char *faces;
    const char *stats;
};

std::optional<SkyPayloadView> ParseSkyPayload(const std::vector<char> &payload)
{
    if (payload.size() < sizeof(TextureCompression::PayloadHeader))
    {
        return std::nullopt;
    }

    TextureCompression::PayloadHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.format >= static_cast<uint32_t>(PixelFormat::Count))
    {
        return std::nullopt;
    }

    const auto format = static_cast<PixelFormat>(header.format);
    const bool known_format = format == PixelFormat::RGBAFloat16 ||
                              format == TextureCompression::SelectHdrFormat(TextureCompression::PlatformFamily);
    if (!known_format || header.width != CubeMapSize || header.height != CubeMapSize || header.mip_count != 1)
    {
        return std::nullopt;
    }

    const size_t face_size = GetImageMipByteSize(format, CubeMapSize, CubeMapSize);
    if (payload.size() != sizeof(header) + 6 * face_size + 2 * sizeof(float) * 3)
    {
        return std::nullopt;
    }

    return SkyPayloadView{.format = format,
                          .faces = payload.data() + sizeof(header),
                          .stats = payload.data() + sizeof(header) + 6 * face_size};
}

std::shared_ptr<Image2DCube> MakeCubeFromFaces(const char *face_data, PixelFormat format, const std::string &cube_name)
{
    const size_t face_size = GetImageMipByteSize(format, CubeMapSize, CubeMapSize);
    std::array<std::unique_ptr<Image2D>, Image2DCube::FaceId::Count> faces;
    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        std::vector<uint8_t> face_payload(face_size);
        std::memcpy(face_payload.data(), face_data + static_cast<size_t>(id) * face_size, face_size);
        if (IsCompressedFormat(format))
        {
            faces[id] =
                std::make_unique<Image2D>(CubeMapSize, CubeMapSize, format, 1, std::move(face_payload), cube_name);
        }
        else
        {
            faces[id] = std::make_unique<Image2D>(CubeMapSize, CubeMapSize, format, face_payload);
            faces[id]->SetName(cube_name);
        }
    }
    return std::make_shared<Image2DCube>(std::move(faces), cube_name);
}

} // namespace

SkyLight::SkyLight() = default;

SkyLight::~SkyLight()
{
    node_->GetScene()->GetRenderProxy()->SetSkyLight(nullptr);
}

CookArtifactKey SkyLight::MasterCookKey(const std::string &sky_map_path)
{
    return {.type = SkyLightCookJob::Type,
            .version = SkyLightCookJob::Version,
            .source_name = std::filesystem::path(sky_map_path).lexically_normal().generic_string(),
            .source_hash = std::nullopt};
}

std::vector<char> SkyLight::CookMasterPayload(const std::string &sky_map_path)
{
    const auto key = MasterCookKey(sky_map_path);
    auto result = Cooker::CookNow(key, [&sky_map_path, &key]() -> std::shared_ptr<CookJob> {
        auto sky_map = std::make_shared<Image2D>();
        if (!sky_map->LoadFromFile(sky_map_path))
        {
            return nullptr;
        }
        return std::make_shared<SkyLightCookJob>(std::move(sky_map), key.source_name);
    });
    return std::move(result.payload);
}

std::shared_ptr<Image2DCube> SkyLight::MakeCubeFromPayload(const std::vector<char> &payload,
                                                           const std::string &sky_map_path)
{
    const auto view = ParseSkyPayload(payload);
    if (!view)
    {
        Log(Error, "invalid sky cube payload for {}", sky_map_path);
        return nullptr;
    }
    return MakeCubeFromFaces(view->faces, view->format, MasterCookKey(sky_map_path).source_name + "_CubeMap");
}

void SkyLight::SetSkyMap(const std::string &file_path)
{
    cook_handle_.reset();
    sky_map_path_ = file_path;

    if (file_path.empty())
    {
        cube_map_.reset();

        if (is_attached_)
        {
            TaskManager::RunInRenderThread([this]() {
                if (GetRenderProxy() != nullptr)
                {
                    RecreateRenderProxy();
                }
            });
        }
        return;
    }

    if (is_attached_)
    {
        RequestCook();
    }
}

void SkyLight::RequestCook()
{
    ASSERT(HasSkyMap());

    auto *scene = node_->GetScene();
    auto scene_task = scene->RegisterAsyncTask();

    cooked_promise_ = std::make_shared<std::promise<void>>();
    cooked_future_ = std::make_shared<TaskFuture<>>(cooked_promise_->get_future());

    const SkyCookFinish finish = [scene_task, promise = cooked_promise_, future = cooked_future_](bool success) {
        scene_task->Complete(success);
        promise->set_value();
        future->OnReady();
    };

    // lookup order: the family transcode (what packaged images and cook-warmed dev pools
    // resolve, keeping every context on the shipped encoding the ground truths reflect),
    // then the fp16 master, then cook the master
    const auto master_key = MasterCookKey(sky_map_path_);
    const auto transcode_key = HdrCubeTranscodeJob::MakeLookupKey(
        SkyLightCookJob::Type, TextureCompression::PlatformFamily, master_key.source_name);

    ProbeArtifact(transcode_key, finish, [this, master_key, finish]() {
        ProbeArtifact(master_key, finish, [this, finish]() { RequestMasterCook(finish); });
    });
}

void SkyLight::ProbeArtifact(const CookArtifactKey &key, const SkyCookFinish &finish, std::function<void()> on_miss)
{
    auto probe_ran = std::make_shared<std::atomic<bool>>(false);
    cook_handle_ = std::make_unique<CookHandle>(
        Cooker::Request(key, nullptr, [this, finish, probe_ran, miss = std::move(on_miss)](CookResult result) {
            probe_ran->store(true);
            if (result.HasPayload())
            {
                DeliverCookedResult(std::move(result), finish);
            }
            else
            {
                miss();
            }
        }));

    cook_handle_->OnDelivered()->Then(
        [finish, probe_ran]() {
            if (!probe_ran->load())
            {
                finish(false);
            }
        },
        TargetThread::Main);
}

void SkyLight::RequestMasterCook(const SkyCookFinish &finish)
{
    const auto source_path = sky_map_path_;
    const auto master_key = MasterCookKey(source_path);
    const auto source_name = master_key.source_name;

    auto delivery_started = std::make_shared<std::atomic<bool>>(false);
    cook_handle_ = std::make_unique<CookHandle>(Cooker::Request(
        master_key,
        [source_path, source_name]() -> std::shared_ptr<CookJob> {
            auto sky_map = std::make_shared<Image2D>();
            if (!sky_map->LoadFromFile(source_path))
            {
                return nullptr;
            }
            return std::make_shared<SkyLightCookJob>(std::move(sky_map), source_name);
        },
        [this, finish, delivery_started](CookResult result) {
            delivery_started->store(true);
            DeliverCookedResult(std::move(result), finish);
        }));

    // On cancellation on_ready does not run, so no render application will retire the
    // request chain. The delivery future still fires and retires it as failed.
    cook_handle_->OnDelivered()->Then(
        [finish, delivery_started]() {
            if (!delivery_started->load())
            {
                finish(false);
            }
        },
        TargetThread::Main);
}

void SkyLight::DeliverCookedResult(CookResult result, const SkyCookFinish &finish)
{
    // a master delivery re-probes the family transcode by content before applying fp16:
    // relocated sources (e.g. usd exports) alias to the shipped encoding this way
    if (result.HasPayload())
    {
        if (const auto view = ParseSkyPayload(result.payload); view && !IsCompressedFormat(view->format))
        {
            auto master_cube = MakeCubeFromFaces(view->faces, view->format, "sky_master_probe");
            auto alias_key = HdrCubeTranscodeJob::MakeLookupKey(
                SkyLightCookJob::Type, TextureCompression::PlatformFamily, MasterCookKey(sky_map_path_).source_name);
            alias_key.source_hash =
                HdrCubeTranscodeJob::MakeSourceHash(master_cube->GetContentHash(), SkyLightCookJob::Version);

            auto master_result = std::make_shared<CookResult>(std::move(result));
            ProbeArtifact(alias_key, finish,
                          [this, master_result, finish]() { ApplyCookedResult(std::move(*master_result), finish); });
            return;
        }
    }

    ApplyCookedResult(std::move(result), finish);
}

void SkyLight::ApplyCookedResult(CookResult result, const SkyCookFinish &finish)
{
    bool applied = false;
    if (result.HasPayload())
    {
        applied = ApplyCookedData(result.payload);
    }
    else
    {
        Log(Error,
            "skymap {} has neither valid cooked content nor a readable source; falling back to a "
            "flat sky",
            sky_map_path_);
    }
    if (!applied)
    {
        cube_map_.reset();
    }

    const bool cook_succeeded = result.IsSuccess() && applied;

    TaskManager::RunInRenderThread([this]() {
        // If the proxy does not exist yet, its initial creation already sees the
        // resolved result.
        if (GetRenderProxy() != nullptr)
        {
            RecreateRenderProxy();
        }
    })->Then([finish, cook_succeeded]() { finish(cook_succeeded); }, TargetThread::Main);
}

const std::shared_ptr<TaskFuture<>> &SkyLight::OnCooked() const
{
    ASSERT(cooked_future_);
    return cooked_future_;
}

std::unique_ptr<RenderProxy> SkyLight::CreateRenderProxy()
{
    auto proxy = std::make_unique<SkyRenderProxy>(cube_map_);

    proxy->SetData(color_);

    if (node_->GetScene()->GetSkyLight() == this)
    {
        node_->GetScene()->GetRenderProxy()->SetSkyLight(proxy.get());
    }
    else
    {
        Log(Warn, "multiple sky lights detected, only the first one will take effect");
    }

    return proxy;
}

void SkyLight::OnAttach()
{
    LightSourceComponent::OnAttach();

    ASSERT(!node_->GetScene()->GetSkyLight());

    node_->GetScene()->SetSkyLight(this);

    if (HasSkyMap() && !cube_map_)
    {
        RequestCook();
    }
}

bool SkyLight::ApplyCookedData(const std::vector<char> &payload)
{
    const auto view = ParseSkyPayload(payload);
    if (!view)
    {
        Log(Error, "sky light artifact for {} is invalid", sky_map_path_);
        return false;
    }

    cube_map_ = MakeCubeFromFaces(view->faces, view->format, MasterCookKey(sky_map_path_).source_name + "_CubeMap");

    const char *ptr = view->stats;
    std::memcpy(sun_brightness_.data(), ptr, sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    std::memcpy(sun_direction_.data(), ptr, sizeof(float) * 3);
    return true;
}
} // namespace sparkle
