#pragma once

#include "core/Path.h"

namespace sparkle
{
class Scene;

// Exports a scene graph to a USDA file that USDLoader can load back.
// See docs/USD.md for the mapping between scene objects and USD prims.
class USDExporter
{
public:
    // Writes the whole scene (nodes, meshes, materials, lights, camera) to output_file.
    // Textures and other binary assets are written to a "textures" directory next to it.
    // output_file must be writable (PathType::Internal or PathType::External) and end with ".usda".
    static bool Export(Scene *scene, const Path &output_file);
};
} // namespace sparkle
