#pragma once

#include "eye/eye_types.h"

namespace bro::engine { class Engine; }

namespace bro::eye {

/// Audits the live rendered page (DOM + actual rendered frame pixels) for
/// contrast failures, text clipping/truncation, overlapping elements,
/// offscreen bounds, undersized hit targets, and frame-level exposure/banding/tiling issues.
EyeAuditResult auditDocument(engine::Engine& engine, const EyeAuditOptions& opts = {});

}  // namespace bro::eye
