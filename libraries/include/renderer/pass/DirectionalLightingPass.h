#pragma once

#include "renderer/pass/ScreenQuadPass.h"

#include "core/Event.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/resource/GBuffer.h"

namespace sparkle
{
class ImageBasedLighting;
class DirectionalLightRenderProxy;
class SkyRenderProxy;

class DirectionalLightingPass : public ScreenQuadPass
{
public:
    struct PassResources
    {
        GBuffer gbuffer;
        CameraRenderProxy *camera;
        DirectionalLightRenderProxy *light;
        RHIResourceRef<RHIImage> depth_texture;
        RHIResourceRef<RHIImage> shadow_map = nullptr;
        ImageBasedLighting *ibl = nullptr;
        SkyRenderProxy *sky_light = nullptr;
    };

    DirectionalLightingPass(RHIContext *ctx, const RHIResourceRef<RHIRenderTarget> &target, PassResources resources);

#pragma region ScreenQuadPass interface

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;
    void SetupPixelShader() override;
    void BindPixelShaderResources() override;
    void Render() override;
    void SetupRenderPass() override;

#pragma endregion

    void SetDirectionalShadow(const RHIResourceRef<RHIImage> &shadow_map);

    void SetIBL(ImageBasedLighting *ibl);

    void SetSkyLight(SkyRenderProxy *sky_light);

private:
    PassResources resources_;

    bool ibl_dirty_ = false;
    std::unique_ptr<EventSubscription> ibl_changed_subscription_;
};
} // namespace sparkle
