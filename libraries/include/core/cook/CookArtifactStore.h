#pragma once

#include "core/cook/CookArtifact.h"

namespace sparkle
{
// Validated persistence for disposable derived data. The store owns manifest and
// domain lookup policy; it does not schedule or execute cook jobs.
class CookArtifactStore
{
public:
    // Resolves packaged content before the writable internal cache. An unresolved key
    // uses its logical identity; a resolved key may reuse identical relocated content.
    // ignore_rebuild_config serves requesters that cannot recook: rebuild_cache must
    // not hide the only remaining copy of a packaged artifact
    [[nodiscard]] static CookPayload Load(const CookArtifactKey &key, bool ignore_rebuild_config = false);

    // Only resolved, non-empty outputs can be persisted.
    static bool Save(const CookArtifactKey &key, const CookPayload &payload);

    // logical identity of an artifact as keyed in cooked/manifest.json
    [[nodiscard]] static std::string GetManifestKey(const CookArtifactKey &key);
};
} // namespace sparkle
