#pragma once

// Scroll layers: a scroll container's content composited as a layer of its
// own. The draw traversal records the content into a buffer the engine owns
// (ScrollLayerHost), in content space: the scroller's content box at scroll 0
// is the origin, so the recording does not depend on the scroll. It records
// the part a window around what shows needs (the visible area and a margin
// on each side, culled against it); the engine rasters that window into a
// surface of its own and the compositor draws it where the scroll puts it,
// under the scroller's clip. Around the content the page is painted as
// before, split at the layer (a layer break): the scroller's own background
// and border below it, what comes after it above.
//
// A scroller is composited this way only when nothing in it needs the page
// around it to paint right (ScrollLayerVeto); otherwise its content is
// painted in place, as before, and the reason is counted
// (perf.stats().paint.scrollLayers).

#include "render/command_buffer.h"
#include "render/layer_source.h"

#include <cstdint>

namespace bro::dom { class Element; }

namespace bro::layout {

// Why a scroll container is painted in place rather than as a scroll layer.
enum class ScrollLayerVeto : uint8_t {
    None = 0,
    Nested,        // inside another scroll layer
    RoundedClip,   // its clip has rounded corners (the compositor clips to rects)
    Effects,       // it or an ancestor has an opacity, transform, filter, blend,
                   // clip-path or perspective the compositor would not apply
    Positioned,    // a positioned or stacking-context box inside it: painted by the
                   // stacking-context walk, outside its content
    Sticky,        // a sticky box inside it
    LayerContent,  // a canvas, WebGL, scene, iframe, terminal or remote view inside it
    Control,       // a form control inside it (it notes where it is drawn in the document)
    Video,         // a <video> inside it
    Count
};
const char* scrollLayerVetoName(ScrollLayerVeto v);

// The engine side of a pass's scroll layers.
class ScrollLayerHost {
public:
    virtual ~ScrollLayerHost() = default;
    // Which set the pass's layers belong to (render::ScrollLayerSource::pass).
    virtual uint32_t scrollLayerPass() const = 0;
    // Called once a pass for each scroller composited as a scroll layer: the
    // buffer its content records into (cleared), for the window (content
    // space, CSS px) the layer's surface holds, placed with its top left at
    // (originX, originY) in the pass's CSS px (the layer quad's origin: its
    // sub-pixel phase is rastered into the surface, so the compositor draws
    // it pixel for pixel). Null: paint it in place.
    virtual render::CommandBuffer* beginScrollLayer(dom::Element* scroller, float winX, float winY, float winW,
                                                    float winH, float originX, float originY) = 0;
};

}  // namespace bro::layout
