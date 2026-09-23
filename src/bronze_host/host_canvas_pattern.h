#pragma once

#include "bronze_host/host_internal.h"

#include <cstdint>
#include <memory>

namespace bro::canvas { struct CanvasPatternData; }

namespace bro::bronze_host {

// A CanvasPattern: the image snapshot, the repetition and the pattern
// transform, shared with every CanvasScene state whose fillStyle/strokeStyle
// currently names it. Shared rather than copied because the web lets a page
// call pattern.setTransform() after assigning the pattern and have the next
// draw see it; the scene resolves the shader at record time, on the JS thread.
struct HostCanvasPattern {
    uint32_t tag = 0x50415454u;  // 'PATT'
    std::shared_ptr<canvas::CanvasPatternData> data;
};

HostCanvasPattern* hostCanvasPatternOf(Value v);

// ctx.createPattern(image, repetition). Answers the CanvasPattern, null for an
// image that has not finished loading, or throws the spec's errors
// (SyntaxError for a bad repetition, InvalidStateError for a broken image,
// a closed ImageBitmap or a zero-sized canvas, TypeError for a non-image).
Value createCanvasPatternValue(std::span<const Value> args);

}  // namespace bro::bronze_host
