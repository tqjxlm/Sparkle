#pragma once

#if ENABLE_VULKAN

#include "core/math/Types.h"
#include "rhi/VulkanRHI.h"

namespace sparkle
{
inline VkTransformMatrixKHR GetVulkanMatrix(const TransformMatrix &mat)
{
    VkTransformMatrixKHR vk_matrix;
    *reinterpret_cast<Vector4 *>(vk_matrix.matrix[0]) = mat.row(0);
    *reinterpret_cast<Vector4 *>(vk_matrix.matrix[1]) = mat.row(1);
    *reinterpret_cast<Vector4 *>(vk_matrix.matrix[2]) = mat.row(2);
    return vk_matrix;
};
} // namespace sparkle

#endif
