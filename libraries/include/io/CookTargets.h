#pragma once

#include "io/TextureCompression.h"

#include <map>
#include <string>
#include <vector>

namespace sparkle
{
// The cook targets the project ships and the texture family each target's packaged content
// carries. The table is generated from cook_targets.json, which build.py and
// dev/ci_matrix.py read directly, so the pipeline and the engine cannot disagree on which
// family a target needs.
class CookTargets
{
public:
    // generated from cook_targets.json
    [[nodiscard]] static const std::map<std::string, TextureCompression::Family> &All();

    [[nodiscard]] static bool Contains(const std::string &target);

    // the target must exist: validate with Contains or Parse first
    [[nodiscard]] static TextureCompression::Family FamilyOf(const std::string &target);

    // every known target name, comma separated, for diagnostics
    [[nodiscard]] static std::string Names();

    // the target this binary cooks for when the config names none. keyed on the framework
    // and the OS: glfw on macOS samples through MoltenVK on an Apple GPU, so it is its own
    // target rather than a desktop one
    [[nodiscard]] static const char *SelfTarget();

    // the texture family this binary's own packaged content carries
    [[nodiscard]] static TextureCompression::Family PlatformFamily();

    // splits a '+'-separated target list, defaulting to SelfTarget() when empty. returns
    // empty after logging when a target is unknown or the list resolves to nothing
    [[nodiscard]] static std::vector<std::string> Parse(const std::string &config);
};
} // namespace sparkle
