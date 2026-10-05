#pragma once

// What a node hands the renderer for one of its images (a material texture,
// a sprite sheet, an HTML panel's pixels): the CPU-side RGBA8 bytes and a
// generation the renderer compares against the one it uploaded. Nodes hold
// no GPU state; the renderer keeps the GPU copy keyed by (node id, slot) and
// re-uploads exactly when the generation it holds is not the current one.
//
// Generations come from one process-wide counter, so a value is never reused:
// clearing a slot and setting new pixels of the same size is still a change,
// and two slots can never be confused for one another.

#include <atomic>
#include <cstdint>
#include <vector>

namespace bro::scene {

/// A fresh, never-before-returned generation (never 0).
inline uint64_t nextResourceGeneration() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct NodeTexture {
    std::vector<uint8_t> rgba;   // width * height * 4, top-left origin
    int width = 0;
    int height = 0;
    uint64_t generation = 0;     // 0 until the slot is first written

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }

    /// Copy `data` (tightly packed RGBA8). A zero extent or null data clears.
    void set(int w, int h, const uint8_t* data) {
        if (w <= 0 || h <= 0 || !data) {
            clear();
            return;
        }
        rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        width = w;
        height = h;
        generation = nextResourceGeneration();
    }

    /// Take ownership of already-packed bytes (`bytes.size() == w * h * 4`).
    void adopt(int w, int h, std::vector<uint8_t>&& bytes) {
        rgba = std::move(bytes);
        width = w;
        height = h;
        generation = nextResourceGeneration();
    }

    void clear() {
        const bool had = !empty();
        rgba.clear();
        width = height = 0;
        if (had) generation = nextResourceGeneration();
    }

    /// The bytes were rewritten in place.
    void touch() { generation = nextResourceGeneration(); }
};

}  // namespace bro::scene
