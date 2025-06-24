#pragma once

#include "io/Material.h"

namespace sparkle
{
class MaterialRenderProxy;

class Material
{
public:
    enum Type : uint8_t
    {
        PBR,
        Dieletric,
        Num,
    };

    virtual ~Material();

    [[nodiscard]] std::unique_ptr<MaterialRenderProxy> CreateRenderProxy();

    void DestroyRenderProxy();

    [[nodiscard]] Type GetType() const
    {
        ASSERT(type_ != Type::Num);
        return type_;
    }

    [[nodiscard]] const MaterialResource &GetRawMaterial() const
    {
        return raw_material_;
    }

    [[nodiscard]] MaterialRenderProxy *GetRenderProxy()
    {
        return render_proxy_;
    }

protected:
    virtual std::unique_ptr<MaterialRenderProxy> CreateRenderProxyInternal() = 0;

    explicit Material(MaterialResource raw_material);

    MaterialResource raw_material_;

    Type type_ = Num;

private:
    MaterialRenderProxy *render_proxy_ = nullptr;
};
} // namespace sparkle
