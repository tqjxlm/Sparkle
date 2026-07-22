#include "scene/component/light/SkyLight.h"

#include <crc32.h>

#include <atomic>

#include "core/Logger.h"
#include "core/cook/CookArtifactStore.h"
#include "core/cook/Cooker.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
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
    static constexpr uint32_t Version = 1;

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

    const size_t face_size = CubeMapSize * CubeMapSize * GetPixelSize(PixelFormat::RGBAFloat16);

    std::vector<char> payload(6 * face_size + 2 * sizeof(float) * 3);
    char *ptr = payload.data();

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        auto face_id = static_cast<Image2DCube::FaceId>(id);
        const auto &face = cube_map.GetFace(face_id);
        std::memcpy(ptr, face.GetRawData(), face_size);
        ptr += face_size;
    }

    std::memcpy(ptr, sun_brightness.data(), sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    std::memcpy(ptr, sun_direction.data(), sizeof(float) * 3);

    return CookJobResult::Success(std::move(payload));
}

CookArtifactKey SkyMapLookupKey(const std::string &sky_map_path)
{
    return {.type = SkyLightCookJob::Type,
            .version = SkyLightCookJob::Version,
            .source_name = std::filesystem::path(sky_map_path).lexically_normal().generic_string(),
            .source_hash = std::nullopt};
}

} // namespace

SkyLight::SkyLight() = default;

SkyLight::~SkyLight()
{
    node_->GetScene()->GetRenderProxy()->SetSkyLight(nullptr);
}

std::string SkyLight::GetCookManifestKey() const
{
    ASSERT(HasSkyMap());
    return CookArtifactStore::GetManifestKey(SkyMapLookupKey(sky_map_path_));
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
    auto cook_succeeded = std::make_shared<std::atomic<bool>>(false);
    auto delivery_started = std::make_shared<std::atomic<bool>>(false);

    const auto source_path = sky_map_path_;
    const auto lookup_key = SkyMapLookupKey(source_path);
    const auto source_name = lookup_key.source_name;

    cook_handle_ = std::make_unique<CookHandle>(Cooker::Request(
        lookup_key,
        [source_path, source_name]() -> std::shared_ptr<CookJob> {
            auto sky_map = std::make_shared<Image2D>();
            if (!sky_map->LoadFromFile(source_path))
            {
                return nullptr;
            }
            return std::make_shared<SkyLightCookJob>(std::move(sky_map), source_name);
        },
        [this, source_path, scene_task, cook_succeeded, delivery_started](CookResult result) {
            delivery_started->store(true);
            cook_succeeded->store(result.IsSuccess());
            if (result.HasPayload())
            {
                ApplyCookedData(result.payload);
            }
            else
            {
                Log(Error,
                    "skymap {} has neither valid cooked content nor a readable source; falling back to a "
                    "flat sky",
                    source_path);
                cube_map_.reset();
            }

            TaskManager::RunInRenderThread([this]() {
                // If the proxy does not exist yet, its initial creation already sees the
                // resolved result.
                if (GetRenderProxy() != nullptr)
                {
                    RecreateRenderProxy();
                }
            })
                ->Then([scene_task, cook_succeeded]() { scene_task->Complete(cook_succeeded->load()); },
                       TargetThread::Main);
        }));

    // On cancellation on_ready does not run, so no render application will complete the
    // scene task. The delivery future still fires and retires that task as failed.
    cook_handle_->OnDelivered()->Then(
        [scene_task, delivery_started]() {
            if (!delivery_started->load())
            {
                scene_task->Complete(false);
            }
        },
        TargetThread::Main);
}

const std::shared_ptr<TaskFuture<>> &SkyLight::OnCooked() const
{
    ASSERT(cook_handle_ && cook_handle_->IsValid());
    return cook_handle_->OnDelivered();
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

void SkyLight::ApplyCookedData(const std::vector<char> &payload)
{
    const size_t face_size = CubeMapSize * CubeMapSize * GetPixelSize(PixelFormat::RGBAFloat16);
    const size_t expected_size = 6 * face_size + 2 * sizeof(float) * 3;

    ASSERT_F(payload.size() == expected_size, "sky light artifact size mismatch. loaded {}. expected {}.",
             payload.size(), expected_size);

    const auto source_name = std::filesystem::path(sky_map_path_).lexically_normal().generic_string();

    cube_map_ =
        std::make_shared<Image2DCube>(CubeMapSize, CubeMapSize, PixelFormat::RGBAFloat16, source_name + "_CubeMap");

    const char *ptr = payload.data();

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        auto face_id = static_cast<Image2DCube::FaceId>(id);
        auto &face = cube_map_->GetFace(face_id);
        std::memcpy(const_cast<uint8_t *>(face.GetRawData()), ptr, face_size);
        ptr += face_size;
    }

    std::memcpy(sun_brightness_.data(), ptr, sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    std::memcpy(sun_direction_.data(), ptr, sizeof(float) * 3);
}
} // namespace sparkle
