#pragma once

#include <atomic>
#include <cstdint>

namespace bro::render {

// Bumped by every custom font a renderer registers (a FontFace or @font-face
// load landing): text recorded before it may now shape with another face, so
// a paint recorded under an older generation is not reused
// (layout/paint_cache.h).
inline std::atomic<uint64_t> g_fontGeneration{0};

inline uint64_t fontGeneration() { return g_fontGeneration.load(std::memory_order_relaxed); }
inline void noteFontRegistered() { g_fontGeneration.fetch_add(1, std::memory_order_relaxed); }

} // namespace bro::render
