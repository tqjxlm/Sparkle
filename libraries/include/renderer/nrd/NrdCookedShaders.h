#pragma once

#include "rhi/RHINrdBackend.h"

#include <cstdint>
#include <map>
#include <string>

namespace sparkle
{
// Cooked ReBLUR shaders loaded from packed resources (cross-compiled at build time by
// shaders/nrd/cook; see docs/Nrd.md). Keyed by a canonical form of
// nrd::PipelineDesc::shaderIdentifier; the version triple guards against building with a stale
// shader cook after an NRD submodule update.
class NrdCookedShaders
{
public:
    bool Load();

    // Resolves a pipeline's shaderIdentifier to a runnable pipeline (reads the cooked shader file).
    // False if the permutation was not cooked or its file is missing.
    bool BuildPipeline(const char *shader_identifier, RHINrdBackend::CookedPipeline &out) const;

    [[nodiscard]] uint32_t VersionMajor() const
    {
        return version_major_;
    }

    [[nodiscard]] uint32_t VersionMinor() const
    {
        return version_minor_;
    }

    [[nodiscard]] uint32_t VersionBuild() const
    {
        return version_build_;
    }

private:
    struct Entry
    {
        std::string file_name;
        std::string entry_point;
        uint32_t threads_per_group[3];
        uint32_t constant_buffer_index;
        // (spirv binding, msl index) pairs; BuildPipeline subtracts NRD's register offsets
        std::vector<std::pair<uint32_t, uint32_t>> srv;
        std::vector<std::pair<uint32_t, uint32_t>> uav;
        std::vector<std::pair<uint32_t, uint32_t>> samplers;
    };

    uint32_t version_major_ = 0;
    uint32_t version_minor_ = 0;
    uint32_t version_build_ = 0;
    std::map<std::string, Entry> entries_;
};
} // namespace sparkle
