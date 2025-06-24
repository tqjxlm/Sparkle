#include "core/math/Ray.h"

#include "core/Logger.h"

namespace sparkle
{
void Ray::Print() const
{
    Log(Info, "origin:\t{}\tdirection:\t{}", utilities::VectorToString(origin_), utilities::VectorToString(direction_));
}
} // namespace sparkle
