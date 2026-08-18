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
class Document;
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
// Host classes
// ---------------------------------------------------------------------------

// One host class: a registered constructor, a prototype minted from it and
// decorated ONCE, and instances born on that prototype. See host_class.cpp for
// why each step is what it is, and host_image.cpp for a converted family read
// end to end.
//
// The win over a bare handle is two things at once: one copy of each method
// per CLASS instead of one per instance, and `x instanceof Name` answering
// true instead of false.
//
// Declare one at file scope per class — the members are pointers, so it is
// constant-initialised and has no static constructor to order.
class HostClass {
public:
    // Mint, decorate, and register `name`. `body` runs for `new Name(...)`;
    // pass nullptr for a class the program may name but not construct, which
    // is what makeBrandConstructor used to be — except that this one brands.
    // Call once, from the family's install function.
    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate);

    // Register a second name for the same constructor (AudioContext and
    // webkitAudioContext; Image and HTMLImageElement).
    void alias(const char* name) const;

    // `class This extends Base`: chain this prototype onto the base's, so an
    // instance inherits both surfaces and answers `instanceof` for both (a
    // File IS a Blob; a GainNode IS an AudioNode). Call AFTER both installs.
    // Prototypes are plain objects, not handle cells, so re-parenting one
    // costs nothing an instance pays for.
    void inherit(const HostClass& base) const;

    // An instance born on this class's prototype, or a bare cell if install()
    // has not run.
    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    // A property on the CONSTRUCTOR, where a class `static` member lands
    // (FileReader.DONE, Node.TEXT_NODE). `name`, `length` and `prototype` are
    // refused by bronze and must not be passed.
    void setStatic(const char* name, Value v) const;

    Value prototype() const;
    Value constructor() const;

private:
    // Heap-allocated and never freed, on purpose: a static destructor would
    // run these after the engine has torn the runtime down. host_class.cpp
    // has the full reasoning.
    ev::Persistent* proto_ = nullptr;
    ev::Persistent* ctor_ = nullptr;
};

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
inline constexpr uint32_t kHostSignalTag = 0x53474E4Cu;   // 'SGNL'
inline constexpr uint32_t kHostMutationObserverTag = 0x4D555442u;  // 'MUTB'
inline constexpr uint32_t kHostResizeObserverTag = 0x52535A42u;    // 'RSZB'
inline constexpr uint32_t kHostVideoEncoderTag = 0x56454E43u;      // 'VENC'
inline constexpr uint32_t kHostGifEncoderTag = 0x47454E43u;        // 'GENC'
inline constexpr uint32_t kHostAudioContextTag = 0x41435458u;      // 'ACTX'
inline constexpr uint32_t kHostAudioNodeTag = 0x414E4F44u;         // 'ANOD'
inline constexpr uint32_t kHostAudioParamTag = 0x41504152u;        // 'APAR'
inline constexpr uint32_t kHostAudioBufferTag = 0x41425546u;       // 'ABUF'
inline constexpr uint32_t kHostPhysicsCharacterTag = 0x50434852u;  // 'PCHR'
inline constexpr uint32_t kHostPhysicsSoftBodyTag = 0x50534259u;   // 'PSBY'
inline constexpr uint32_t kHostNavGridTag = 0x4E564744u;  // 'NVGD'
inline constexpr uint32_t kHostNavMeshTag = 0x4E564D53u;  // 'NVMS'
inline constexpr uint32_t kHostAgentTag   = 0x41474E54u;  // 'AGNT'
inline constexpr uint32_t kHostWebSocketTag = 0x57534F43u; // 'WSOC'

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

// A DEADLINE for host work, on the same table and the same clock the app's own
// setTimeout uses — so a host-scheduled abort and an app-scheduled one are
// ordered against each other rather than against two different notions of now.
// The callback is host memory freed on the main thread as the entry is erased,
// never from a finalizer, so unlike a JS listener it may hold Persistents.
// Answers the id, which nothing needs yet; the symmetry with clearTimeout is
// the point of returning it rather than a promise of one.
int32_t hostSetTimeout(std::function<void()> task, double delayMs);

// ---------------------------------------------------------------------------
// Events (host_events.cpp)
// ---------------------------------------------------------------------------

// A private list stored ON a host object under `key`: a plain object with
// numeric keys and a `length`, because the embed API builds plain objects and
// has no array constructor. This is the shape every "things to call later" list
// in this layer takes, and it is on the object rather than in host memory for
// the reason the GC rule gives — a host-side table would need Persistents owned
// by the object's finalizer, which is the one thing a finalizer may not own.
// The snapshot is taken whole before anything runs, so a list mutated by one
// entry does not disturb the run in progress.
void hostListAppend(ev::Persistent& obj, const std::string& key, Value v);
std::vector<ev::Persistent> hostListSnapshot(const ev::Persistent& obj,
                                             const std::string& key);

// The `on<type>` slot plus the addEventListener list, for the host objects that
// fire events. Both live as ordinary properties ON THE OBJECT — see above.
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
// `{type, bubbles, cancelable, detail}` object rather than a `new
// CustomEvent(...)`. That was forced when nothing here could be built on a
// chosen prototype; it no longer is — embed::makeHandle takes one, and
// host_image.cpp works the shape end to end — so a real CustomEvent class
// is buildable and merely unwritten. The descriptor stays until it is: it is
// the documented channel and compiled code already speaks it. Answers
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
    ev::Persistent datasetObj;
    bool hasStyle = false;
    bool hasClassList = false;
    bool hasComputed = false;
    bool hasDataset = false;
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
void installNodeTree(ObjectBuilder& b);

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

// ---------------------------------------------------------------------------
// Property traps (host_proxy.cpp)
// ---------------------------------------------------------------------------

// A live view whose keys are not known when it is built: `el.style`,
// `el.dataset`, the computed declaration, `localStorage`. Each callback is
// optional; one left empty behaves as the absence of that capability (no keys,
// no membership, dropped writes) rather than as an error.
//
// `get` returns true when it HANDLED the key — false means "no such property",
// which is how a style object distinguishes an unset CSS property (handled,
// answers "") from a name that is not a property at all.
struct HostProxyTraps {
    // Consulted before `get` and `has`, and never enumerated: the object's
    // fixed method surface, which on the web would sit on a prototype. May be
    // undefined for a view that has none.
    Value methods = ev::undefined();
    std::function<bool(const std::string& key, Value& out)> get;
    std::function<void(const std::string& key, Value v)> set;
    std::function<bool(const std::string& key)> has;
    std::function<std::vector<std::string>()> ownKeys;
    std::function<void(const std::string& key)> remove;
};

// The proxy itself. Builds its own empty target — see host_proxy.cpp for why
// the target must stay empty for the 10.5 invariants to stay vacuous.
Value makeHostProxy(HostProxyTraps traps);

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

// Installs Element/HTMLElement as a real class. Must run BEFORE any element
// value is built, or those elements are born on the bare handle shape and
// carry no members at all.
void installElementGlobals();

// The node state behind a host node VALUE (an element, text or comment
// wrapper). This is what a member on a shared prototype uses in place of a
// captured pointer: one copy of the method serves every node, so the receiver
// is the only thing that says which node the call is about.
HostNodeState* hostNodeStateOfValue(Value v);

// An element and nothing more (host_element.cpp); a canvas (dom_globals.cpp).
Value makePlainElementValue(dom::Element* el);
Value makeCanvasElementValue(dom::Element* el);

// Fullscreen element tracking (host_element.cpp owns it).
void setHostFullscreenElement(dom::Element* el);
dom::Element* hostFullscreenElement();

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
// AbortController / AbortSignal (host_abort.cpp)
// ---------------------------------------------------------------------------

void installAbortGlobals();

// The payload behind an AbortSignal. `aborted` is duplicated as a JS property
// so the program can read `signal.aborted`; this copy is what host code checks,
// because a fetch deciding whether to reject must not depend on a property the
// app is free to overwrite.
struct HostAbortSignal {
    uint32_t tag = kHostSignalTag;  // must be first — see the tag note above
    bool aborted = false;
};

// The signal behind a value, or nullptr for anything that is not one. This is
// how `init.signal` is recognised: an arbitrary object with an `aborted`
// property is NOT a signal, and treating one as if it were would make a typo
// look like a working abort.
const HostAbortSignal* hostAbortSignalOf(Value v);

// A fresh signal, not yet aborted.
Value makeAbortSignalValue();

// Abort `signal` with `reason` — the whole algorithm, in one place because
// three callers need it: controller.abort(), AbortSignal.timeout's deadline,
// and a source signal propagating into an AbortSignal.any() composite. Setting
// `reason` to undefined means "the default", an AbortError. Idempotent: a
// signal already aborted keeps its first reason and fires nothing, which is
// what makes abort() safe to call from a cleanup path that may run twice.
void hostAbortSignal(Value signal, Value reason);

// `{name, message}` — what this layer rejects and throws with where the web
// throws a DOMException. A real DOMException class is buildable now (see
// host_image.cpp for the shape) and is not written; `e.name === 'AbortError'`
// is the check real code writes and it answers correctly either way.
Value hostMakeDomError(const char* name, const std::string& message);

// ---------------------------------------------------------------------------
// MutationObserver / ResizeObserver (host_observers.cpp)
// ---------------------------------------------------------------------------

void installObserverGlobals();

// Hand every observer what it has accumulated: for a MutationObserver the
// records queued since the last delivery, for a ResizeObserver the targets
// whose box has changed since it last looked. One call per observer with all
// of them, because an observer that rebuilds a view from its entries needs
// them together.
//
// Called once per frame from the bronze frame seam, AFTER requestAnimationFrame
// — so a mutation or a resize caused by a frame callback is reported in the
// frame that caused it — and before the closing microtask drain, so the promise
// jobs an observer starts run in the same checkpoint as everything else's.
// Anything queued from inside a callback waits for the next frame, which is
// what stops an observer that changes what it observes from re-entering itself.
void deliverHostObservers();

// ---------------------------------------------------------------------------
// DOMParser (host_parser.cpp)
// ---------------------------------------------------------------------------

void installParserGlobal();

// A full document surface — the queries, the factories, the element accessors —
// bound to `doc` rather than to whatever the engine is currently showing. The
// `document` global is the one wrapper NOT built this way; dom_globals.cpp's
// documentFor() says why. Defined there, beside the builder it shares.
Value hostDocumentValue(dom::Document* doc);

// ---------------------------------------------------------------------------
// VideoEncoder / GifEncoder (host_video.cpp)
// ---------------------------------------------------------------------------

// Registers both names in EVERY build. Without video compiled in they are
// registered as `undefined` rather than left out, because a manifest name the
// host never registers is a process abort and not a catchable miss — the file
// carries the rule and where it comes from.
void installVideoGlobals();

// ---------------------------------------------------------------------------
// XMLHttpRequest (host_xhr.cpp)
// ---------------------------------------------------------------------------

void installXhrGlobal();

// ---------------------------------------------------------------------------
// fetch (host_fetch.cpp)
// ---------------------------------------------------------------------------

void installFetchGlobal();

// ---------------------------------------------------------------------------
// Audio (host_audio.cpp)
// ---------------------------------------------------------------------------

void installAudioGlobals();
Value makeBrandConstructor(const char* name);

// ---------------------------------------------------------------------------
// Physics (host_physics.cpp)
// ---------------------------------------------------------------------------

void installPhysicsGlobals();
void drainPhysicsContactEvents();

// ---------------------------------------------------------------------------
// AI & Navigation (host_ai.cpp)
// ---------------------------------------------------------------------------

void installAIGlobals();

// ---------------------------------------------------------------------------
// Networking & Remote Transport (host_net.cpp)
// ---------------------------------------------------------------------------

void installNetGlobals();
void drainNetEvents();
Value makeBroNetValue();

}  // namespace bro::bronze_host
