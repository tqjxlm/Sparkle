#pragma once

#include <string>
#include <vector>

namespace sparkle
{
class RHIContext;
struct RenderConfig;

// Build-time cook driver: plans every artifact the requested targets need, runs the jobs through
// the Cooker, derives the per-family HDR cube transcodes from the fp16 masters the run produced,
// and writes the cook_products.json the packaging stage reads. RHI-free by contract — a context,
// when one exists, only accelerates IBL integration. Returns 0 on success.
[[nodiscard]] int RunCookPipeline(const std::vector<std::string> &targets, const std::string &scene_path,
                                  RHIContext *rhi, const RenderConfig &render_config);
} // namespace sparkle
