#include "scene/material/Material.h"

#include "renderer/proxy/MaterialRenderProxy.h"

namespace sparkle
{
Material::Material(MaterialResource raw_material) : raw_material_(std::move(raw_material))
{
    ASSERT(!raw_material_.name.empty());
}

Material::~Material()
{
    ASSERT(!render_proxy_);
}

std::unique_ptr<MaterialRenderProxy> Material::CreateRenderProxy()
{
    ASSERT(!render_proxy_);
    auto proxy = CreateRenderProxyInternal();
    render_proxy_ = proxy.get();
    return proxy;
}

void Material::DestroyRenderProxy()
{
    if (render_proxy_)
    {
        render_proxy_->Destroy();
        render_proxy_ = nullptr;
    }
}
} // namespace sparkle
