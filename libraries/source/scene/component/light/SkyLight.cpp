#include "scene/component/light/SkyLight.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/Timer.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "scene/Scene.h"

namespace sparkle
{
constexpr unsigned CubeMapSize = 1024;

SkyLight::SkyLight() = default;

SkyLight::~SkyLight()
{
    node_->GetScene()->GetRenderProxy()->SetSkyLight(nullptr);
}

void SkyLight::SetSkyMap(const std::string &file_path)
{
    sky_map_ = std::make_unique<Image2D>();
    if (!sky_map_->LoadFromFile(file_path))
    {
        Log(Error, "failed to load skymap {}. will fall back to sky light", file_path);
        sky_map_ = nullptr;
    }

    Cook();
}

std::unique_ptr<RenderProxy> SkyLight::CreateRenderProxy()
{
    auto proxy = std::make_unique<SkyRenderProxy>(cube_map_.get());

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
}

void SkyLight::Cook()
{
    ASSERT(sky_map_);

    if (TryLoadCache())
    {
        cooked_ = true;
        return;
    }

    Log(Info, "Cooking sky map {}", sky_map_->GetName());
    Timer timer;

    cube_map_ = std::make_unique<Image2DCube>(CubeMapSize, CubeMapSize, PixelFormat::RGBAFloat16,
                                              sky_map_->GetName() + "_CubeMap");

    std::array<Scalar, Image2DCube::FaceId::Count> max_brightness_per_face;
    std::array<Vector3, Image2DCube::FaceId::Count> max_brightness_dir_per_face;
    std::array<Vector3, Image2DCube::FaceId::Count> subtracted_color_per_face;

    std::ranges::fill(max_brightness_per_face, 0.f);
    std::ranges::fill(max_brightness_dir_per_face, Zeros);
    std::ranges::fill(subtracted_color_per_face, Zeros);

    std::vector<std::shared_ptr<TaskFuture<void>>> cube_map_tasks(Image2DCube::FaceId::Count);

    cooked_row_count_ = 0;

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        auto face_id = static_cast<Image2DCube::FaceId>(id);
        auto &this_face = cube_map_->GetFace(face_id);

        cube_map_tasks[id] =
            TaskManager::RunInWorkerThread([this, face_id, &this_face, &max_brightness_per_face,
                                            &max_brightness_dir_per_face, &subtracted_color_per_face]() {
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
                        if (brightness > SkyRenderProxy::MaxIBLBrightness)
                        {
                            // TODO(tqjxlm): MaxBrightness is not analytically correct.
                            Vector3 subtracted_color = utilities::ClampLength(color, SkyRenderProxy::MaxBrightness) -
                                                       utilities::ClampLength(color, SkyRenderProxy::MaxIBLBrightness);

                            constexpr float Numerator = 4.f * (1.f / CubeMapSize) * (1.f / CubeMapSize);
                            float solid_angle = Numerator / std::pow(1 + u * u + v * v, 1.5f);

                            subtracted_color_per_face[face_id] += subtracted_color * solid_angle;
                        }
                    }

                    cooked_row_count_++;
                }
            });
    }

    // kick off the log timer
    TaskManager::RunInMainThread([this]() { LogCookStatus(); });

    TaskManager::OnAll(cube_map_tasks)->Wait();

    Scalar max_brightness = 0.f;
    Vector3 max_brightness_dir;
    sun_brightness_ = Zeros;
    for (unsigned i = 0; i < Image2DCube::FaceId::Count; i++)
    {
        if (max_brightness_per_face[i] > max_brightness)
        {
            max_brightness = max_brightness_per_face[i];
            max_brightness_dir = max_brightness_dir_per_face[i];
        }

        sun_brightness_ += subtracted_color_per_face[i];
    }

    Eigen::Quaternion<Scalar> q = Eigen::Quaternion<Scalar>::FromTwoVectors(Front, max_brightness_dir);

    sun_direction_ = q.toRotationMatrix().eulerAngles(0, 1, 2);

    Log(Info, "sky map cook ok. took time {}s. max brightness {}. max brightness direction {}. sun brightness {}",
        timer.ElapsedSecond(), max_brightness, utilities::VectorToString(utilities::ToDegree(sun_direction_)),
        utilities::VectorToString(sun_brightness_));

    cooked_ = true;

    SaveCache();
}

void SkyLight::LogCookStatus() const
{
    // TODO(tqjxlm): it's better to use some kind of repeat timer
    float progress = static_cast<float>(cooked_row_count_.load()) / (CubeMapSize * Image2DCube::FaceId::Count) * 100.f;
    Logger::LogToScreen("SkyLight::Cook", fmt::format("Cooking sky map {:.1f}%", progress));

    if (!cooked_)
    {
        // keep logging until we are finished
        TaskManager::RunInMainThread([this]() { LogCookStatus(); }, false);
    }
    else
    {
        Logger::LogToScreen("SkyLight::Cook", "");
    }
}

std::string SkyLight::GetCachePath() const
{
    return "cached/skylight/" + sky_map_->GetName() + ".cache";
}

bool SkyLight::TryLoadCache()
{
    auto *file_manager = FileManager::GetNativeFileManager();
    auto file_path = GetCachePath();

    auto data = file_manager->Read(Path::Internal(file_path));
    if (data.empty())
    {
        return false;
    }

    const size_t face_size = CubeMapSize * CubeMapSize * GetPixelSize(PixelFormat::RGBAFloat16);
    const size_t expected_size = 6 * face_size + 2 * sizeof(float) * 3;

    if (data.size() != expected_size)
    {
        Log(Warn, "sky light cache size mismatch. loaded {}. expected {}.", data.size(), expected_size);
        return false;
    }

    cube_map_ = std::make_unique<Image2DCube>(CubeMapSize, CubeMapSize, PixelFormat::RGBAFloat16,
                                              sky_map_->GetName() + "_CubeMap");

    const char *ptr = data.data();

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

    Log(Info, "loaded sky light cache from {}", file_path);

    return true;
}

void SkyLight::SaveCache() const
{
    auto *file_manager = FileManager::GetNativeFileManager();
    auto file_path = GetCachePath();

    const size_t face_size = CubeMapSize * CubeMapSize * GetPixelSize(PixelFormat::RGBAFloat16);
    const size_t total_size = 6 * face_size + 2 * sizeof(float) * 3;

    std::vector<char> data(total_size);
    char *ptr = data.data();

    for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
    {
        auto face_id = static_cast<Image2DCube::FaceId>(id);
        const auto &face = cube_map_->GetFace(face_id);
        std::memcpy(ptr, face.GetRawData(), face_size);
        ptr += face_size;
    }

    std::memcpy(ptr, sun_brightness_.data(), sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    std::memcpy(ptr, sun_direction_.data(), sizeof(float) * 3);

    auto written_path = file_manager->Write(Path::Internal(file_path), data);

    if (written_path.empty())
    {
        Log(Error, "failed to save sky light cache: {}", file_path);
    }
    else
    {
        Log(Info, "saved sky light cache to {}", written_path);
    }
}

} // namespace sparkle
