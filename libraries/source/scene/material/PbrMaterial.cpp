#include "scene/material/PbrMaterial.h"

#include "renderer/proxy/PbrMaterialRenderProxy.h"

namespace sparkle
{
std::unique_ptr<MaterialRenderProxy> PbrMaterial::CreateRenderProxyInternal()
{
    return std::make_unique<PbrMaterialRenderProxy>(raw_material_);
}
} // namespace sparkle
