#pragma once

#include "eye/eye_types.h"

namespace bro::engine { class Engine; }

namespace bro::eye {

/// Structural DOM layout grammar audit: alignment rails & near-miss drifts,
/// sibling spacing regularity, Gestalt inner/outer ratio, awkward narrow wraps,
/// and near-touch "tangent" collisions.
EyeLayoutAnalysis analyzeLayout(engine::Engine& engine, const EyeComposeOptions& opts = {});

}  // namespace bro::eye
