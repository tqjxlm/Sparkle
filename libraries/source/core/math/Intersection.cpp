#include "core/math/Intersection.h"

#include "core/Logger.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"

namespace sparkle
{
void Intersection::Print() const
{
    if (!IsHit())
    {
        Log(Info, "no hit");
    }
    else
    {
        Log(Info, "hit at: [{}] | position: {} | normal: {} | tangent: {} | tex_coord: {}", primitive_->GetName(),
            utilities::VectorToString(GetLocation()), utilities::VectorToString(normal_),
            utilities::VectorToString(tangent_), utilities::VectorToString(tex_coord_));
    }
}
} // namespace sparkle
