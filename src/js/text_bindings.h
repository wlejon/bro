#pragma once

#include <quickjs.h>

namespace bro::engine { class Engine; }

namespace bro::js {

// Installs `bro.text` — a DIAGNOSTIC window onto the render layer's text
// shaping. It exists so the cluster map can be asserted from a test without a
// separate C++ test binary, and so a shaping regression shows up as a failing
// assertion rather than as slightly-wrong pixels nobody looks at.
//
// This is deliberately NOT an app-facing text API. Two consequences:
//   * offsets here are UTF-8 BYTES, the render layer's own domain, not the
//     UTF-16 code units every app-facing DOM offset speaks. Byte offsets are
//     what the cluster map is expressed in, and translating them here would
//     hide exactly the thing the tests need to see.
//   * it reports what the shaper did, not what CSS says should happen.
//
// Surface (see tests/style/test_text_shaping.js):
//   bro.text.shape(text, { family, size, weight, italic, letterSpacing,
//                          wordSpacing })
//     -> { text, glyphCount, width, clusters: [{ start, end, x, advance,
//          glyphs, rtl }] }
//   bro.text.byteOffsetToX(text, opts, byteOffset)
//     -> { x, isLeadingEdge, secondary?: { x, isLeadingEdge } }
//   bro.text.xToByteOffset(text, opts, x)  -> byte offset
//   bro.text.clusterRange(text, opts, byteOffset) -> { start, end }
//   bro.text.cacheStats() -> { hits, misses }
void installTextBindings(JSContext* ctx, engine::Engine* engine);

} // namespace bro::js
