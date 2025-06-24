#pragma once

#include <cstdint>

namespace sparkle
{
struct PbrConfig
{
    uint32_t mode;
    uint32_t use_ssao;
    uint32_t use_ibl_diffuse;
    uint32_t use_ibl_specular;
};
} // namespace sparkle
