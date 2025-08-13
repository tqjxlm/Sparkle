#include "scene/component/light/SkyLight.h"

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
SkyLight::SkyLight() = default;

SkyLight::~SkyLight() = default;

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
    Log(Info, "Cooking sky map {}", sky_map_->GetName());
    Timer timer;

    // TODO(tqjxlm): cache cook results
    ASSERT(sky_map_);

    constexpr unsigned CubeMapSize = 1024;

    cube_map_ = std::make_unique<Image2DCube>(CubeMapSize, CubeMapSize, PixelFormat::RGBAFloat16,
                                              sky_map_->GetName() + "_CubeMap");

    std::array<Scalar, 6> max_brightness_per_face;
    std::array<Vector3, 6> max_brightness_dir_per_face;
    std::array<Vector3, 6> subtracted_color_per_face;

    std::ranges::fill(max_brightness_per_face, 0.f);
    std::ranges::fill(max_brightness_dir_per_face, Zeros);
    std::ranges::fill(subtracted_color_per_face, Zeros);

    std::vector<std::shared_ptr<TaskFuture<void>>> cube_map_tasks(6);

    for (unsigned id = 0; id < 6; id++)
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
                }
            });
    }

    TaskManager::OnAll(cube_map_tasks)->Wait();

    Scalar max_brightness = 0.f;
    Vector3 max_brightness_dir;
    sun_brightness_ = Zeros;
    for (unsigned i = 0; i < 6; i++)
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
}

} // namespace sparkle
