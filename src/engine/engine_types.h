#pragma once

#include "dom/node_handle.h"
#include "engine/dom_undo.h"
#include <bromath/color.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::dom { class Document; class Element; }
namespace bro::webgl { class WebGL2RenderingContext; }
namespace bro::scene { class SceneGraph; }

namespace bro::engine {

/// Insets reserved by engine UI around the app document.
struct ContentInsets { int top = 0, right = 0, bottom = 0, left = 0; };

/// Loaded custom font data for registering on layout thread's renderer
struct LoadedFont {
    std::string family;
    std::vector<char> data;
    int weight;
    bool italic;
};

/// Selection geometry snapshot — computed on the main thread from live
/// Range/Node pointers, then consumed by the raster thread without
/// touching the DOM.
struct SelectionSnapshot {
    struct Rect { float x, y, w, h; };
    std::vector<Rect> rects;
    bool hasCaret = false;
    float caretX = 0, caretY = 0, caretHeight = 0;
    bromath::Color highlight{0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<Rect> compUnderlines;
    bromath::Color compColor{0.0f, 0.0f, 0.0f, 1.0f};
};

/// WebGL contexts (owned by engine, associated with canvas elements)
struct WebGLEntry {
    std::unique_ptr<webgl::WebGL2RenderingContext> context;
    dom::Element* element = nullptr;  // non-owning
};

#if BRO_WITH_3D
struct SceneGraphEntry {
    std::unique_ptr<scene::SceneGraph> graph;
    dom::Element* element = nullptr;
    dom::Document* document = nullptr;
    uint32_t elementId = 0;
};
#endif

/// Touch contact table entry
struct TouchContact {
    uint64_t fingerId = 0;      // SDL finger id / injector-chosen id
    int pointerId = 0;          // W3C PointerEvent.pointerId (≥ 2)
    bool primary = false;       // first contact of the current set
    float x = 0.0f, y = 0.0f;   // latest position (window space)
    float downX = 0.0f, downY = 0.0f;  // position at touch-down
    float pressure = 1.0f;
    bool moved = false;         // travelled past the tap slop
    bool compatSuppressed = false; // preventDefault on pointerdown/touchstart
    dom::ElementHandle startTarget;
};

/// Two-finger gesture recognition state
struct GestureState {
    bool active = false;
    uint64_t fingerA = 0, fingerB = 0;  // founding contacts
    float startDist = 1.0f;             // finger distance at start (px, >= 1)
    float startAngle = 0.0f;            // atan2 angle at start (radians)
    float scale = 1.0f;                 // last reported scale
    float rotation = 0.0f;              // last reported rotation (degrees, continuous past 180)
    float cx = 0.0f, cy = 0.0f;         // last centroid (window space)
    dom::ElementHandle target;          // hit target of the start centroid
};

/// In-progress IME composition inside a contenteditable host of the app document.
struct EditableComposition {
    bool active = false;
    dom::TextNodeHandle node;   // text node carrying the preedit
    dom::ElementHandle host;    // contenteditable host (event target)
    bool createdNode = false;   // `node` was created for this composition
    int start = 0;              // byte offset of the preedit in `node`
    int length = 0;             // byte length of the current preedit
    std::string preedit;        // current preedit text
    std::string hostBefore;     // host innerHTML before the composition
    DomUndoStack::Sel selBefore;// selection before the composition
    bool replacedSelection = false;  // composition started over a selection
};

} // namespace bro::engine
