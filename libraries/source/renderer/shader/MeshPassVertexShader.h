#pragma once

#include "rhi/RHIShader.h"

namespace sparkle
{
class DepthOnlyVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(DepthOnlyVertexShader, RHIShaderStage::Vertex, "shaders/standard/depth_only.vs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(mesh, RHIShaderResourceReflection::ResourceType::UniformBuffer)

    END_SHADER_RESOURCE_TABLE
};

class StandardVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(StandardVertexShader, RHIShaderStage::Vertex, "shaders/standard/mesh.vs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)
    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(mesh, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    END_SHADER_RESOURCE_TABLE
};
} // namespace sparkle
