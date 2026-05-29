#pragma once

#include "render/renderer.h"
#include <include/core/SkBlendMode.h>
#include <include/core/SkRefCnt.h>
#include <span>

class SkImageFilter;

namespace bro::render {

// Map a CSS mix-blend-mode to the matching SkBlendMode. Unknown/Normal → kSrcOver.
SkBlendMode toSkBlendMode(BlendMode mode);

// Build a Skia SkImageFilter chain from a list of CSS filter descriptors.
// Returns nullptr for an empty input. Used by SkiaRenderer and RasterRenderer
// to implement Renderer::saveLayerWithFilter without leaking Skia types into
// the public Renderer interface or DrawTraversal.
sk_sp<SkImageFilter> BuildSkImageFilterChain(std::span<const CssFilterParams> filters);

} // namespace bro::render
