#include "application/NativeView.h"

#include "core/FileManager.h"
#include "core/math/Utilities.h"

namespace sparkle
{
Mat2 NativeView::GetRotationMatrix(WindowRotation rotation)
{
    switch (rotation)
    {
    case NativeView::WindowRotation::Portrait:
        return Eigen::Rotation2D<Scalar>(utilities::ToRadian(0.f)).matrix();
    case NativeView::WindowRotation::Landscape:
        return Eigen::Rotation2D<Scalar>(utilities::ToRadian(90.f)).matrix();
    case NativeView::WindowRotation::ReversePortrait:
        return Eigen::Rotation2D<Scalar>(utilities::ToRadian(180.f)).matrix();
    case NativeView::WindowRotation::ReverseLandscape:
        return Eigen::Rotation2D<Scalar>(utilities::ToRadian(270.f)).matrix();
    default:
        UnImplemented(rotation);
        return Mat2::Identity();
    }
}

NativeView::NativeView()
{
    file_manager_ = FileManager::CreateNativeFileManager();
}

NativeView::~NativeView() = default;

#if ENABLE_VULKAN
bool NativeView::CreateVulkanSurface(void * /*in_instance*/, void * /*out_surface*/)
{
    UnImplemented();
    return false;
}

void NativeView::GetVulkanRequiredExtensions(std::vector<const char *> & /*required_extensions*/)
{
    UnImplemented();
}
#endif
} // namespace sparkle
