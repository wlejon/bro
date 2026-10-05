#pragma once

// The engine's half of the <terminal> compositor layers (layout/term_layer.h):
// the surfaces each terminal's recording is replayed into and the image the
// compositor places. Every terminal is one entry, keyed by its layer id, the
// handle the app pass's layer break names (render::TerminalLayerSource).
//
// Threads, as for iframe sub-documents: record() runs on the main thread in
// the raster-idle record block (and in a headless capture), the only place
// the entry table changes; replay() runs on whichever thread replays the
// frame (the raster thread windowed, the main thread headless), the only
// one that draws into the surfaces; published() is read by the main thread's
// compositor, through PublishedFrame's lock.

#include "engine/published_frame.h"
#include "render/skia_gpu.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::layout { struct TermLayer; }
namespace bro::render { class SkiaRenderer; }

namespace bro::engine {

// BRO_TERMINAL_LAYER=0 draws every <terminal> inline in the page's paint
// instead, re-recording the page for each terminal frame: the comparison the
// layer is measured against (docs/terminal-api.js, "Performance").
bool terminalLayersEnabled();

class TerminalLayers {
public:
    TerminalLayers();
    ~TerminalLayers();

    // Main thread, raster idle: record each live terminal whose paint
    // changed (pump() flagged it, or its box or `scale` did), and retire the
    // entries of terminals that are gone.
    void record(float scale);
    // Replay the recordings made since the last replay into their surfaces
    // and publish them. Unchanged terminals keep their published image.
    void replay(render::SkiaRenderer* renderer);
    // The image to composite for a layer id (null: none yet / gone).
    const PublishedFrame* published(uint64_t id) const;
    // Surfaces released (raster thread teardown).
    void releaseAll();

    uint64_t replays() const { return replays_; }

private:
    struct Entry {
        std::shared_ptr<layout::TermLayer> layer;
        render::LayerSurface surface, spare;
        int surfW = 0, surfH = 0;
        uint64_t replayedGeneration = 0;
        PublishedFrame published;
    };
    std::unordered_map<uint64_t, std::unique_ptr<Entry>> entries_;
    std::vector<std::unique_ptr<Entry>> retired_;  // freed by the next replay
    uint64_t replays_ = 0;
};

} // namespace bro::engine
