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
// and border below it, what comes after it above. A scroll the window still
// holds records nothing in the layer and rasters it again only when it moves
// the layer by a fraction of a device pixel.
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

// Where a scroller's layer is in a pass. The layer break's quad has the
// content origin, not the scroll: the compositor adds the scroll it shows, so
// the pass around the layer records the same whatever the scroll.
struct ScrollLayerPlace {
    float contentX = 0, contentY = 0;  // the content box at scroll 0, in the pass's CSS px
    float scrollX = 0, scrollY = 0;
    // What shows, in content space: the padding box where the scroll has it.
    float viewX = 0, viewY = 0, viewW = 0, viewH = 0;
};

// What the traversal does with a layer's content this pass.
struct ScrollLayerRecord {
    render::CommandBuffer* buffer = nullptr;  // null: paint it in place
    // The recording the layer holds stands (nothing under the scroller was
    // stamped since, and its window still holds the view): nothing to record.
    bool reuse = false;
    // The window to record, in content space: the view and a margin round it.
    float winX = 0, winY = 0, winW = 0, winH = 0;
};

// The engine side of a pass's scroll layers.
class ScrollLayerHost {
public:
    virtual ~ScrollLayerHost() = default;
    // Which set the pass's layers belong to (render::ScrollLayerSource::pass).
    virtual uint32_t scrollLayerPass() const = 0;
    // Called once a pass for each scroller composited as a scroll layer.
    // `mayReuse` false: what was recorded before cannot stand (the pass's
    // span cache was emptied), record it again.
    virtual ScrollLayerRecord beginScrollLayer(dom::Element* scroller, const ScrollLayerPlace& place,
                                               bool mayReuse) = 0;
    // After recording into it: whether the recording may stand in a later
    // pass (false: something in it changes with no stamp, an animated or
    // decoding image).
    virtual void endScrollLayer(dom::Element* scroller, bool reusable) = 0;
};

}  // namespace bro::layout
