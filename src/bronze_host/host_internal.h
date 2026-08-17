#pragma once

// The non-GL half of the bronze host layer: the frame seam every host-provided
// global hangs off, and the three things the files on it must share — the
// error funnel, the frame clock, and the main-thread task queue.
//
// gl_internal.h is the other half. It owns ObjectBuilder and the argument
// readers, because that is where the value boundary was first drawn; a file
// here includes both headers and says so at the top.
//
// THE GC RULE, restated because most of what follows exists to obey it: a
// Value held across an allocating embed call is stale. Host state that must
// outlive such a call lives in an ev::Persistent — and a Persistent must never
// be owned by anything a HANDLE FINALIZER destroys. A finalizer runs
// mid-collection and may not call back into the embed API (embed.h says so),
// and ~Persistent IS the embed API. That single rule is why the payload
// structs below are plain host memory and every callback lives as an ordinary
// property on the object instead.

#include "embed/embed.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bro::engine { class Engine; }
namespace bro::dom {
class Element;
class Event;
class Node;
class DocumentFragment;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// gl_internal.h owns it; a file that only registers properties does not need
// the GL headers to say so.
struct ObjectBuilder;

// ---------------------------------------------------------------------------
// Handle tags
// ---------------------------------------------------------------------------

// Every host object with a C++ payload is an embed handle, and every unwrap in
// this layer reaches it through the same embed::handleData — which answers a
// void* with no type on it. So each payload struct starts with a uint32_t tag
// in the same position as GlCell::kind (gl_internal.h), carrying a value no GL
// kind uses: an Image handed to idOf(v, GlCell::Texture) reads kHostImageTag,
// fails the kind compare and answers 0, and a WebGLTexture handed to
// hostImageOf reads a kind of 1..8 and answers nullptr. The alternative — each
// unwrap trusting that it is only ever passed its own cells — is the shape of
// bug that reads a Shape* as a Value.
inline constexpr uint32_t kHostElementTag = 0x454C454Du;  // 'ELEM'
inline constexpr uint32_t kHostImageTag = 0x494D4147u;    // 'IMAG'
inline constexpr uint32_t kHostXhrTag = 0x58485220u;      // 'XHR '
inline constexpr uint32_t kHostFetchTag = 0x52455350u;    // 'RESP'
inline constexpr uint32_t kHostHeadersTag = 0x48454144u;  // 'HEAD'
inline constexpr uint32_t kHostRequestTag = 0x52455155u;  // 'REQU'
inline constexpr uint32_t kHostBlobTag = 0x424C4F42u;     // 'BLOB'
inline constexpr uint32_t kHostReaderTag = 0x46524452u;   // 'FRDR'

// ---------------------------------------------------------------------------
// The error funnel and the frame clock (dom_globals.cpp)
// ---------------------------------------------------------------------------

// Where an exception out of compiled code ends up: reported to the engine log
// and dropped, so one broken callback never silences its siblings or tears the
// loop down.
void reportBronzeError(const char* origin, Value thrown);

// The Engine this layer was installed on, or nullptr before install. Every
// file here reaches the engine through it rather than through a second copy
// of the pointer.
engine::Engine* hostEngine();

// The host object this layer handed the program for `el` — the canvas value
// from document.createElement('canvas') — or undefined for an element it
// never wrapped. This is what makes `event.target === canvas` true inside a
// compiled listener, and it is identity, not a rebuild: the same Value the
// program already holds.
Value hostValueForElement(dom::Element* el);

// Milliseconds of SCALED engine time since installWebHostGlobals: the
// accumulated Engine::onFrame deltas. This is the clock rAF timestamps and
// performance.now() answer from, and the one timer deadlines are measured
// against — so it is frozen while bro.time is paused and virtual under
// headless advanceTime, exactly as the clock bro's own JS gets is. A compiled
// app and a JS app in the same engine therefore agree about what "now" is.
double hostClockMs();

// ---------------------------------------------------------------------------
// The main-thread task queue (host_timers.cpp)
// ---------------------------------------------------------------------------

// Where a host binding puts work that must not run inside the call that
// produced it: an image's load event, an XHR's completion. Drained once per
// frame at the top of the bronze frame seam, BEFORE requestAnimationFrame —
// which is where the web runs a load event relative to the rendering steps, and
// what lets a texture that finished decoding be uploaded by the same frame that
// learns about it.
//
// Main thread only, by construction rather than by locking: nothing in this
// layer runs off it (host_image.cpp says why its decode is synchronous), so a
// mutex here would be a claim about threads that is not true. A future producer
// that really is off-thread must add the lock AND state what it protects.
void postHostTask(std::function<void()> task);
void drainHostTasks();

// ---------------------------------------------------------------------------
// Events (host_events.cpp)
// ---------------------------------------------------------------------------

// The `on<type>` slot plus the addEventListener list, for the host objects that
// fire events. Both live as ordinary properties ON THE OBJECT — see the GC rule
// above: a host-side listener table would need Persistents owned by the
// object's finalizer, which is the one thing a finalizer may not own.
void addHostListener(ev::Persistent& obj, const std::string& type, Value fn);
void removeHostListener(ev::Persistent& obj, const std::string& type, Value fn);

// Fire `type` at `target`: the `on<type>` property first, then every
// addEventListener listener in registration order, each called with `target` as
// the receiver and a minimal `{type, target}` event object. A listener list
// mutated during dispatch does not disturb the run in progress (the list is
// snapshotted first) — three.js's ImageLoader removes its own listeners from
// inside them, so this is load-bearing, not defensive.
//
// This layer does NOT model the web's single registration-ordered listener list
// spanning both spellings: an `onload` assigned after an addEventListener('load')
// still runs first. Nothing three.js does can see the difference, and the
// alternative is a host-side registration counter that only a finalizer could
// own.
void dispatchHostEvent(ev::Persistent target, const std::string& type);

// ---------------------------------------------------------------------------
// DOM events (host_dom_events.cpp)
// ---------------------------------------------------------------------------

// addEventListener / removeEventListener / dispatchEvent on a host object
// whose real identity is a dom::Element, wired to the ENGINE's listener
// registry (dom::Element::addEventListener) rather than to a list of this
// layer's own. That is the whole point: the engine already runs one dispatch
// walk that merges native and interpreted listeners in registration order
// (js/event_dispatch.cpp), so a compiled listener registered here fires from
// a real click, in the right phase, beside the page's own listeners — instead
// of from a second dispatch system that nothing would ever call.
//
// `source` is resolved at each call rather than captured as a pointer: the
// document's element target is documentElement, which does not exist yet when
// the globals are registered. `what` names the object in diagnostics.
using ElementSource = std::function<dom::Element*()>;
void installElementEventTarget(ObjectBuilder& b, ElementSource source,
                               const char* what);

// Hand `evt` to one compiled listener as PLAIN DATA — a fresh object carrying
// the fields for the event's kind (coordinates, key, button, deltas, the
// string detail of a CustomEvent), the target identity, and the three
// propagation methods, which write through to the live dom::Event for the
// duration of this call and refuse afterwards. Nothing from either heap
// crosses; every field is copied. A throw out of the listener is reported
// through reportBronzeError under `origin` and dispatch continues.
void callBronzeListener(const ev::Persistent& fn, const ev::Persistent& thisObj,
                        dom::Event& evt, const char* origin);

// `dispatchEvent(desc)` from compiled code, where `desc` is a plain
// `{type, bubbles, cancelable, detail}` object — bronze cannot build a value
// on a chosen prototype, so there is no `new CustomEvent` to construct here
// (the same limit that makes `img instanceof Image` false). Answers
// `!defaultPrevented`, as the web's dispatchEvent does, or a pending
// TypeError for a descriptor without a string `type`.
Value hostDispatchToElement(ElementSource source, const char* what, Value desc);
Value hostDispatchToWindow(Value desc);

// ---------------------------------------------------------------------------
// The node registry (host_element.cpp owns it; host_node.cpp shares it)
// ---------------------------------------------------------------------------

// One entry per DOM node this layer has ever wrapped, and the thing every
// accessor on a wrapper captures. It is reached from a wrapper through
// embed::handleData, which is why `tag` is first (see the tag note above).
//
// `node` is what the wrapper IS; `el` is the same pointer when that node is an
// element and nullptr otherwise. Keeping both is what lets the element surface
// guard on `st->el` alone: a text wrapper never has those accessors installed,
// but a stale one that somehow did would answer inert rather than reinterpret a
// TextNode* as an Element*.
//
// Both go null when the node is freed (Document::addNodeFreedObserver). The
// entry itself is never freed while the program might still hold the wrapper —
// it holds Persistents, and ~Persistent is an embed call, which the GC rule
// above forbids a handle finalizer from making.
struct HostNodeState {
    uint32_t tag = kHostElementTag;  // must be first — see the tag note above
    dom::Node* node = nullptr;
    dom::Element* el = nullptr;
    ev::Persistent jsObj;
    ev::Persistent styleObj;
    ev::Persistent classListObj;
    ev::Persistent computedObj;
    bool hasStyle = false;
    bool hasClassList = false;
    bool hasComputed = false;
};

// The entry for `node`, created on first ask. Never null for a non-null node.
HostNodeState* hostNodeStateFor(dom::Node* node);

// A fresh object that is already a node handle — what every wrapper in this
// layer is built on, so hostNodeOf() can recover the dom::Node* from a value
// the program hands back to appendChild.
Value makeNodeHandleObject(dom::Node* node);

// THE wrapper for `node`, whatever kind it is: an element through
// hostElementValue, a text or comment node through host_node.cpp, a fragment.
// Null for nullptr, so a parent/sibling lookup can be handed straight in.
Value hostNodeValue(dom::Node* node);

// The dom::Node behind any wrapper this layer made, or nullptr. The node-level
// counterpart of hostElementOf: appendChild takes this, because a text node is
// a legal child and is not an element.
dom::Node* hostNodeOf(Value v);

// ---------------------------------------------------------------------------
// Text, comment and fragment nodes (host_node.cpp)
// ---------------------------------------------------------------------------

// The CharacterData surface — `data`, `nodeValue`, `textContent`, `length`,
// and the five mutators — over a TextNode or a CommentNode. They share every
// method and no base class, so the wrapper is written once against the pair.
Value makeCharacterDataValue(dom::Node* node);

// A DocumentFragment: a parent that holds children and vanishes into the tree
// when inserted. Nothing but the node surface, which is all a fragment has.
Value makeFragmentValue(dom::DocumentFragment* frag);

// The NODE half of the tree surface — parentNode, childNodes, the child edges
// and siblings, the four mutators, contains, cloneNode, remove. Installed on
// every wrapper kind, because every one of them is a Node. The element-only
// extras (`children`, `firstElementChild`, querySelector…) stay in
// installElementCore beside it.
void installNodeTree(ObjectBuilder& b, HostNodeState* st);

// Insert `child` under `parent` before `ref` (append when `ref` is null),
// unparenting it first and spilling a DocumentFragment's children in its place
// — the one insertion path all four mutators funnel through, so the fragment
// rule and the reparent rule are stated once.
void hostInsertNode(dom::Node* parent, dom::Node* child, dom::Node* ref);

// ---------------------------------------------------------------------------
// Elements (host_element.cpp)
// ---------------------------------------------------------------------------

// A REAL JS array of `count` items, `make(i)` supplying each. Real, because
// what an app does with `children` or `querySelectorAll` is iterate it —
// `for…of`, `Array.from`, `.map` — and an object with numeric keys and a
// `length` has neither Array.prototype nor an iterator, so every one of those
// is a TypeError at the call site rather than an empty result.
//
// `make(i)` runs with the array already rooted and its result is stored
// immediately, which is what keeps this inside the GC rule: a pre-built
// std::vector<Value> would be stale from its second element onwards.
Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

// THE element wrapper for `el` — built on first ask, the same value every time
// after that, because identity is what a UI tests (`event.target === this.dom`).
// Answers null for nullptr, so it can be handed a parent/sibling lookup result
// directly.
Value hostElementValue(dom::Element* el);

// The dom::Element behind a wrapper, or nullptr for any other value. This is
// what makes `parent.appendChild(child)` possible: the wrapper is an embed
// handle whose data is its registry entry.
dom::Element* hostElementOf(Value v);

// getComputedStyle(el): the LIVE resolved-value declaration for an element
// wrapper — used widths off the layout box, lengths in px, colours as rgb() —
// resolved by layout::computedProperty, the same function bro's own JS
// bindings answer from. Anything that is not an element wrapper answers an
// object whose properties are all the empty string, which is what those
// bindings do too.
Value hostComputedStyleFor(Value elValue);

// Record a wrapper this file did not build — dom_globals.cpp's canvas, which
// is an element plus a drawing buffer — so it keeps its identity in the
// registry like any other.
void noteHostElementValue(dom::Element* el, Value v);

bool isCanvasTag(const std::string& tag);

// A fresh object that is ALREADY an element handle — what every element
// wrapper in this layer must be built on, so hostElementOf() can recover the
// dom::Element* from a value the program hands back to appendChild.
Value makeElementHandleObject(dom::Element* el);

// The whole element surface — identity, style, classList, attributes, the
// tree, geometry, focus, and the event target — onto an object under
// construction. Shared, so a canvas is an element that also has a drawing
// buffer rather than a separate kind of thing that happens to look like one.
void installElementCore(ObjectBuilder& b, dom::Element* el);

// An element and nothing more (host_element.cpp); a canvas (dom_globals.cpp).
Value makePlainElementValue(dom::Element* el);
Value makeCanvasElementValue(dom::Element* el);

// ---------------------------------------------------------------------------
// Timers (host_timers.cpp)
// ---------------------------------------------------------------------------

// setTimeout / clearTimeout / setInterval / clearInterval, on hostClockMs().
void installTimerGlobals();

// Fire every timer whose deadline has passed, in (deadline, id) order — HTML's
// order for same-deadline timers is creation order. Called once per frame from
// the bronze frame seam, before requestAnimationFrame, which is where bro's own
// loop ticks js::Timers relative to rAF (engine_frame.cpp step 2 vs step 3a).
void fireHostTimers(double nowMs);

// ---------------------------------------------------------------------------
// Image (host_image.cpp)
// ---------------------------------------------------------------------------

void installImageGlobal();

// One image element, as the document's element factory spells it:
// createElement('img') and createElementNS(ns, 'img') both answer one of these,
// and so does `new Image()`.
Value makeImageValue();

// The decode result behind an Image value, or nullptr for anything that is not
// one. The bytes are HOST memory (a std::vector owned by the value's handle
// cell), NOT heap bytes — so unlike embed::typedArrayInfo's pointer this one
// survives a bronze allocation and stays valid until the value is collected.
// That is what lets the texture upload path read width/height through embed
// calls and only then hand the pixels to GL.
struct HostImage {
    uint32_t tag = kHostImageTag;  // must be first — see the tag note above
    std::string src;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // RGBA8, top-down (row 0 = top); empty if broken
    bool complete = false;      // the load settled, either way
    bool ok = false;            // ... and it settled as a success
};
const HostImage* hostImageOf(Value v);

// ---------------------------------------------------------------------------
// Platform odds and ends (host_platform.cpp)
// ---------------------------------------------------------------------------

// btoa/atob, queueMicrotask, screen, alert/confirm/prompt, and the DOM
// interface NAMES libraries test for (`typeof Node !== "undefined"`,
// `x instanceof HTMLInputElement`). No state, no frame seam.
void installPlatformGlobals();

// ---------------------------------------------------------------------------
// Blob / File / FileReader / URL (host_file.cpp)
// ---------------------------------------------------------------------------

void installFileGlobals();

// The bytes behind a Blob or File value, or nullptr for anything else. HOST
// memory (a std::vector owned by the value's handle cell), not heap bytes — so
// unlike embed::typedArrayInfo's pointer this one survives a bronze allocation
// and stays valid until the value is collected. hostImageOf has the same
// contract and for the same reason.
struct HostBlob {
    uint32_t tag = kHostBlobTag;  // must be first — see the tag note above
    std::vector<uint8_t> bytes;
    std::string type;           // the MIME type; may be empty
    bool isFile = false;        // a File is a Blob with a name
    std::string name;
    double lastModified = 0;    // ms since the epoch, as the web reports it
};
const HostBlob* hostBlobOf(Value v);

// A Blob value over `bytes`. The bytes are MOVED IN: a Blob is immutable on
// the web, so there is never a second owner to keep in step.
Value makeBlobValue(std::vector<uint8_t> bytes, std::string type);

// ---------------------------------------------------------------------------
// XMLHttpRequest (host_xhr.cpp)
// ---------------------------------------------------------------------------

void installXhrGlobal();

// ---------------------------------------------------------------------------
// fetch (host_fetch.cpp)
// ---------------------------------------------------------------------------

void installFetchGlobal();

}  // namespace bro::bronze_host
