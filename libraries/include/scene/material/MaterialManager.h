#pragma once

#include "core/Exception.h"

#include <memory>
#include <vector>

namespace sparkle
{
class Material;
struct MaterialResource;

class MaterialManager
{
public:
    enum MetalType : uint8_t
    {
        GOLD,
        IRON,
        BRONZE,
        ALUMINIUM,
        SILVER,
        COUNT
    };

    MaterialManager();

    ~MaterialManager();

    static std::unique_ptr<MaterialManager> CreateInstance();

    static MaterialManager &Instance()
    {
        ASSERT(instance_);
        return *instance_;
    }

    void Destroy();

    template <class T> std::shared_ptr<T> GetOrCreateMaterial(const MaterialResource &material_resource)
    {
        // TODO(tqjxlm): cache and reuse
        auto new_material = std::make_shared<T>(material_resource);
        return new_material;
    }

    std::shared_ptr<Material> GetDefaultMaterial()
    {
        return default_material_;
    }

    std::shared_ptr<Material> GetRandomMetalMaterial();

    const auto &GetMetalMaterials()
    {
        return metals_;
    }

    [[nodiscard]] bool IsValid() const
    {
        return valid_;
    }

private:
    std::vector<std::shared_ptr<Material>> metals_;
    std::shared_ptr<Material> default_material_;

    bool valid_ = true;

    static MaterialManager *instance_;
};
} // namespace sparkle
