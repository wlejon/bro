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
#include "runtime/bigint.h"
#include "runtime/heap.h"
#include "dom/event_target.h"
#include "bronze_host/host_numeric.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::engine {
class Engine;
struct GamepadState;
}  // namespace bro::engine
namespace bro::physics {
class PhysicsWorld;
}
namespace bro::dom {
class Document;
class Element;
class Event;
class Node;
class DocumentFragment;
class ShadowRoot;
struct AbsoluteRect;
}  // namespace bro::dom
namespace bronze::embed {
Value setPrototype(Value obj, Value proto);
}  // namespace bronze::embed

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

    void init(const char* name, const std::function<void(ObjectBuilder&)>& decorate) {
        install(name, 0, nullptr, decorate);
    }

    template <typename T>
    Value createInstance(std::unique_ptr<T> cell, ev::Finalize when = ev::Finalize::InSweep) const {
        if (!cell) return ev::undefined();
        T* raw = cell.release();
        return make(raw, [](void* p) { delete static_cast<T*>(p); }, when);
    }

    // Register a second name for the same constructor (Image and
    // HTMLImageElement).
    void alias(const char* name) const;

    // Bind this HostClass to an existing constructor and its prototype by name.
    void bind(const char* name);

    // `class This extends Base`: chain this prototype onto the base's, so an
    // instance inherits both surfaces and answers `instanceof` for both (an
    // HTMLDivElement IS an HTMLElement). Call AFTER both installs.
    // Prototypes are plain objects, not handle cells, so re-parenting one
    // costs nothing an instance pays for.
    void inherit(const HostClass& base) const;

    // An instance born on this class's prototype, or a bare cell if install()
    // has not run.
    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    // A property on the CONSTRUCTOR, where a class `static` member lands
    // (Node.TEXT_NODE). `name`, `length` and `prototype` are
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
inline constexpr uint32_t kHostVideoEncoderTag = 0x56454E43u;  // 'VENC'
inline constexpr uint32_t kHostGifEncoderTag   = 0x47454E43u;  // 'GENC'

// ---------------------------------------------------------------------------
// The error funnel and the frame clock (dom_globals.cpp)
// ---------------------------------------------------------------------------

// Where an exception out of compiled code ends up: reported to the engine log
// and dropped, so one broken callback never silences its siblings or tears the
// loop down.
void reportBronzeError(const char* origin, Value thrown);

// The browser's "report the exception" step for `thrown`: window.onerror,
// then an ErrorEvent at the window's `error` listeners (host_error_events.cpp).
// True when the page cancelled it (onerror returned true, or a listener
// called preventDefault), which is the caller's cue to skip its log line.
// A throw from inside an error handler is never re-dispatched. ALLOCATES.
bool hostDispatchUncaughtError(Value thrown);

// `window.postMessage(message, targetOrigin, transfer)` for every realm
// (host_window_message.cpp): clones at the call, delivers a `message`
// MessageEvent at the CALLING realm's window as a later task. ALLOCATES.
Value makeWindowPostMessage();

// The origin every realm of the page has (`location.origin`).
inline constexpr const char* kHostPageOrigin = "bro://app";

// The parsed tail of a postMessage call: (message, targetOrigin, transfer),
// (message, {targetOrigin, transfer}) and — with `legacyTransferArray` —
// (message, transferArray). `deliver` is false when targetOrigin names
// another origin (the message is then dropped silently, per spec).
struct PostMessageTarget {
    std::string targetOrigin;
    ev::Persistent transfer;
    bool deliver = true;
};
// False with `thrown` set to a pending SyntaxError for a bad targetOrigin.
bool parsePostMessageArgs(std::span<const Value> args, const char* what,
                          bool legacyTransferArray, PostMessageTarget& out, Value& thrown);
// structuredClone `message` with `transfer`; the MessagePorts in the list come
// back as `portsOut` (an array). False with `thrown` pending on a clone error.
bool cloneForPostMessage(const ev::Persistent& message, const ev::Persistent& transfer,
                         ev::Persistent& dataOut, ev::Persistent& portsOut, Value& thrown);
// Deliver a `message` MessageEvent at one window: the realm `scopeId` (0 =
// main; a window host id; an iframe document's scope) whose document is
// `doc` (ignored for 0). `onmessage` first, then that document's window
// listeners. A window that has gone away receives nothing. ALLOCATES.
void deliverWindowMessageEvent(uint64_t scopeId, dom::Document* doc, const ev::Persistent& data,
                               const ev::Persistent& ports, const ev::Persistent& source,
                               const std::string& origin);

// The text `reportBronzeError` prints for a thrown value: its `stack` when a
// program set one, else `Name: message` for an Error, else its JSON for any
// other object, else ToString of the primitive. bronze itself records no
// stack and no source position on an Error (runtime/exception.h), so this
// is the whole of what a report can say about WHAT was thrown; the caller
// supplies WHERE (the script, the seam). ALLOCATES.
std::string thrownValueText(Value thrown);

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
Value describeTarget(dom::Element* el);

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
// produced it: an image's load event. Drained once per
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
// walk that dispatches listeners in registration order
// (dom/event_dispatch.cpp), so a compiled listener registered here fires from
// a real click, in the right phase, beside the page's own listeners — instead
// of from a second dispatch system that nothing would ever call.
//
// `source` is resolved at each call rather than captured as a pointer: the
// document's element target is documentElement, which does not exist yet when
// the globals are registered. `what` names the object in diagnostics.
using ElementSource = std::function<dom::Element*()>;
void installElementEventTarget(ObjectBuilder& b, ElementSource source,
                               const char* what);
dom::ListenerOptions readOptions(Value optV);

// Hand `evt` to one compiled listener as PLAIN DATA — a fresh object carrying
// the fields for the event's kind (coordinates, key, button, deltas, the
// string detail of a CustomEvent), the target identity, and the three
// propagation methods, which write through to the live dom::Event for the
// duration of this call and refuse afterwards. Nothing from either heap
// crosses; every field is copied. A throw out of the listener is reported
// through reportBronzeError under `origin` and dispatch continues.
void callBronzeListener(const ev::Persistent& fn, const ev::Persistent& thisObj,
                        dom::Event& evt, const char* origin,
                        dom::Document* docOverride = nullptr);

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
// element.focus()'s steps (host_element_interaction.cpp): focus, blur,
// focusin, focusout. A no-op for an inert element (outside a modal dialog).
void hostFocusElement(dom::Element* el);
// The same at one document's window — a secondary window's, an iframe's.
Value hostDispatchToWindowOf(dom::Document* doc, Value desc);

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
// An <img>'s decoded pixels, defined further down with the rest of the image
// path. Declared here because an img ELEMENT carries one: the wrapper is a
// node like any other element's, and the decoder hangs off its state rather
// than replacing it (host_element_image.cpp).
struct HostImage;

struct HostNodeState {
    uint32_t tag = kHostElementTag;  // must be first — see the tag note above
    dom::Node* node = nullptr;
    dom::Element* el = nullptr;
    dom::ShadowRoot* shadowRoot = nullptr;
    ev::Persistent jsObj;
    ev::Persistent styleObj;
    ev::Persistent classListObj;
    ev::Persistent computedObj;
    ev::Persistent datasetObj;
    std::unordered_map<std::string, uint64_t> inlineHandles;
    std::unordered_map<std::string, ev::Persistent> inlineFns;
    bool hasStyle = false;
    bool hasClassList = false;
    bool hasComputed = false;
    bool hasDataset = false;
    bool fromImageConstructor = false;
    // <dialog> only (host_dialog.cpp): `returnValue`, and whether it was
    // opened by showModal() — the "is modal" flag show() and showModal()
    // check each other against.
    bool dialogModal = false;
    std::string dialogReturnValue;
    // Non-null only for an <img>: its src, its size and its RGBA. Owned here
    // so it dies with the node's entry, which is what the registry's
    // unique_ptr already guarantees.
    std::unique_ptr<HostImage> image;
};

// ---------------------------------------------------------------------------
// Style & dataset decomposition (host_element_style.cpp / dataset.cpp / forms.cpp)
// ---------------------------------------------------------------------------
Value makeStyleObject(HostNodeState* st);
Value makeComputedStyleObject(HostNodeState* st);
void decorateElementStyle(ObjectBuilder& b);

Value makeDatasetObject(HostNodeState* st);
Value makeClassListObject(HostNodeState* st);
void decorateElementDataset(ObjectBuilder& b);

void decorateElementForms(ObjectBuilder& b);
void decorateElementValidity(ObjectBuilder& b);
void decorateElementMutate(ObjectBuilder& b);
void decorateElementInteraction(ObjectBuilder& b);
void decorateElementGeometry(ObjectBuilder& b);
dom::AbsoluteRect borderBoxOf(dom::Element* el);

// A DOMRect-shaped plain object — x/y/width/height plus the four edges.
Value makeHostRectValue(double x, double y, double w, double h);

// The WebIDL `DOMString?` / [LegacyNullToEmptyString] conversion, which is
// what `textContent`, `innerHTML`, `data` and `nodeValue` are declared as:
// both null and undefined become "".
//
// It has to be spelled out because `ev::isObject(null)` is FALSE in bronze, so
// the `!isObject(v)` guard every setter in this layer uses to reject objects
// lets null straight through to ev::toUtf8 — which stringifies it, and the
// page ends up showing the word "null" where a browser shows nothing. The
// widget spelling that hits it is ordinary:
//     this.label.textContent = value;   // value optional, often omitted
std::string hostNullableString(Value v);

// Install the constraint-validation pattern tester (layout::form_validation)
// on top of this realm's RegExp. Called once, when the host globals go in.
void installHostPatternTester();
Value makeLiveHTMLCollection(dom::Element* root, dom::Document* fixed, std::string selector);

// The <img> half of the element surface (host_element_image.cpp). `Image` is
// an element CLASS here, so the members live on its prototype and an img
// wrapper is born on that instead of on Element's — which is why the handle
// comes from here rather than from host_element.cpp's element class.
Value makeImageElementHandle(dom::Element* el);
void primeImageFromMarkup(dom::Element* el);

// ---------------------------------------------------------------------------
// Storage & Gamepad (dom_storage.cpp / dom_gamepad.cpp)
// ---------------------------------------------------------------------------
Value makeLocalStorageValue();
Value makeScreenValue();
Value makeNavigatorValue();
Value buildGamepadSnapshot(const engine::GamepadState& gp);
void installGamepadButtonGlobals();
const HostClass& gamepadEventHostClass();

// `navigator` as a host global (host_navigator.cpp): makeNavigatorValue's
// object plus `clipboard` and `getBattery()`.
void installNavigatorGlobal();

// `performance` (host_performance.cpp): `now()` on hostClockMs, and the User
// Timing marks and measures on the same clock.
Value makePerformanceValue();

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
Value makeFragmentValue(dom::Node* frag);

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

inline Value makeEmptyArray() {
    return hostArrayOf(0, [](size_t) { return ev::undefined(); });
}

inline Value hostMakeDomError(const char* name, const std::string& message) {
    // Each allocation below may move every value made before it, so each
    // lands in a Persistent, and each string is made in its own statement
    // before the receiver it is stored on is read.
    ev::Persistent msgVal(ev::fromUtf8(message));
    ev::Persistent errObj;
    auto g = ev::globalValue("Error");
    if (g.found) {
        Value arg = msgVal.get();
        ev::CallResult res = ev::construct(g.value, std::span<const Value>(&arg, 1));
        errObj.set(res.thrown ? ev::createObject() : res.value);
    } else {
        errObj.set(ev::createObject());
    }
    if (name && *name) {
        ev::Persistent nameVal(ev::fromUtf8(name));
        errObj.set(ev::setProperty(errObj.get(), "name", nameVal.get()));
    }
    return errObj.get();
}

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

    // A CALLABLE view. The four live views this file was written for are data,
    // and an empty object target is all they need; a view standing in for a
    // FUNCTION in another engine is not, because [[Call]] and [[Construct]]
    // are the target's and no trap can conjure them. So such a caller supplies
    // its own callable target — an unnamed host function, so that it carries
    // no own `name`/`length` for the 10.5 invariants to check the traps
    // against — and the two traps that forward through it.
    Value target = ev::undefined();
    std::function<Value(Value thisValue, std::span<const Value> args)> apply;
    std::function<Value(std::span<const Value> args)> construct;
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
bool isImgTag(const std::string& tag);

// The class every element is born on. Exposed so that a SUBCLASS — `Image`,
// today the only one — can chain its prototype onto it.
const HostClass& elementHostClass();

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
// the bronze frame seam, before requestAnimationFrame.
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
    uint64_t loadId = 0;
    std::shared_ptr<uint64_t> activeLoadToken;
    std::vector<ev::Persistent> pendingDecodePromises;
};
const HostImage* hostImageOf(Value v);

// Resolve `src` and decode it into `img`, leaving `img.complete` true either
// way and `img.ok` true only on success. Shared by `new Image()` and by an
// <img> element, because a texture must not depend on which of the two the
// page happened to build (host_image.cpp). A relative `src` resolves against
// `doc`'s own base path when one is given — an <img> in a system panel or an
// <iframe> names a file beside ITS markup, not the app's — and against the
// app directory otherwise.
void loadHostImage(HostImage& img, const std::string& src, const dom::Document* doc = nullptr,
                   dom::Element* target = nullptr);

// The one decoder every image byte stream in this layer goes through: the
// bitmap codecs (broimage), then WebP, then SVG rasterized at its intrinsic
// size. Answers straight-alpha RGBA8, top-down; false with `err` set when no
// decoder accepted the bytes (host_image.cpp).
bool decodeHostImageBytes(const uint8_t* bytes, size_t len,
                          int& outW, int& outH, std::vector<uint8_t>& outRgba,
                          std::string* err);

// ---------------------------------------------------------------------------
// Platform odds and ends (host_platform.cpp)
// ---------------------------------------------------------------------------

// queueMicrotask, screen, alert/confirm/prompt, and the DOM interface NAMES
// libraries test for (`typeof Node !== "undefined"`,
// `x instanceof HTMLInputElement`). No state, no frame seam.
void installPlatformGlobals();

// ---------------------------------------------------------------------------
// DOMParser (host_parser.cpp)
// ---------------------------------------------------------------------------

void installParserGlobal();

// The `CSS` namespace: supports() and escape() (host_css_namespace.cpp).
void installCssNamespace();

// A detached document parsed from `html`, owned for the life of the process
// like every DOMParser result; what document.implementation.createHTMLDocument
// hands back as well.
dom::Document* parseIntoNewDocument(const std::string& html);

// A full document surface — the queries, the factories, the element accessors —
// bound to `doc` rather than to whatever the engine is currently showing. The
// `document` global is the one wrapper NOT built this way; dom_globals.cpp's
// documentFor() says why. Defined there, beside the builder it shares.
Value makeDocumentValue(dom::Document* fixed);
Value hostDocumentValue(dom::Document* doc);

// ---------------------------------------------------------------------------
// Brand constructors (dom_globals.cpp) and touch (host_touch.cpp)
// ---------------------------------------------------------------------------

Value makeBrandConstructor(const char* name);
void installTouchGlobals();

// ---------------------------------------------------------------------------
// Vendor globals (host_vendor_globals.cpp)
// ---------------------------------------------------------------------------

void installVendorGlobals();

// ---------------------------------------------------------------------------
// brokit (host_brokit.cpp): the Node half — require, fs, path, os,
// child_process, process — and the web half — fetch, URL, Blob, encoding,
// base64, AbortController, WebSocket, streams, crypto, indexedDB, TreeWalker,
// EventTarget, MessageChannel. What it skips, and why, is at its top.
// ---------------------------------------------------------------------------

void installBrokitGlobals(engine::Engine& engine);

// A brokit `File` read off disk (host_file_path.cpp): bytes, the MIME type
// its extension implies, `lastModified`, and the non-standard `.path` — the
// object a drop and an <input type=file> pick hand a page. `undefined` for
// a path that cannot be read. ALLOCATES.
Value makeFileFromPath(const std::string& path);

// The same, or for an unreadable path a plain `{name, path, size: 0, type}`
// so a file list never has a hole in it. ALLOCATES.
Value makeFileOrDescriptorFromPath(const std::string& path);

// One pass over brokit's polled completions (`__brokit_fetch_tick` and its
// siblings). hostFrame runs it as a host-task step; headless advanceTime and
// flush run it so a pending fetch resolves inside the call a test makes.
void pumpBrokitTicks();

// Whether brokit still has a fetch, socket or watcher in flight — the realm is
// not idle while it does (host_gc.cpp).
bool brokitHasPendingWork();

// ---------------------------------------------------------------------------
// bro's own bronze-compiled JavaScript (host_js_modules.cpp), and the host
// hooks the observer module runs over (host_observer_hooks.cpp)
// ---------------------------------------------------------------------------

// `__bro_observers`: the mutation-record take and the frame tick that
// js/observers.js builds MutationObserver, ResizeObserver and
// IntersectionObserver on. Registered by installObserversModule before the
// module's entry runs.
void installObserverHooks();

// The observer module's per-frame pass (resize, then intersection), fired
// from the frame seam after requestAnimationFrame and its microtask
// checkpoint. A no-op until the module has registered its callback.
void fireHostObserverFrame();

// Lift a value some compiled JS (or a sibling installer) ASSIGNED onto
// `globalThis` into bronze's host-global registry, so a compiled read of the
// bare name answers with the same object. A no-op for a name globalThis
// lacks.
void adoptGlobalProperty(const char* name);

// Enter each compiled module and lift what it defined on globalThis into the
// host-global registry: MutationObserver/ResizeObserver/IntersectionObserver
// and their entry classes; the UI event classes (MouseEvent, KeyboardEvent,
// ...) over brokit's Event; `__bro_net_sync` (a factory over the bro.net
// primitives); `__bro_image_gpu` (colormap, fbm2D).
void installObserversModule();
void installEventsModule();
// The global object's prototype chain (host_global_proto.cpp): globalThis ->
// <ctorName>.prototype -> EventTarget.prototype (Object.prototype without
// one), with Symbol.toStringTag = <ctorName>, and <ctorName> registered as
// a global whose `new` is an Illegal constructor TypeError. Without it the
// global object has no prototype and String(window) throws. "Window" for the
// page realm, "DedicatedWorkerGlobalScope" for a Worker's.
void installGlobalPrototype(const char* ctorName);
void installNetSyncModule();
void installImageGpuModule();
// js/bro_core.js: the public bro.time / bro.window / bro.settings /
// bro.appDir surface and the panels' __bro.*, assembled over the natives
// under __bro_native. Entered by installBroRoots (host_bro_root.cpp) after
// the roots and natives are registered; lifts nothing.
void installBroCoreModule();
// The 3D family (js/physics.js, terrain, clipmap, tile_world, lighting,
// gizmo, animation, scene, impostor) and js/net.js / js/motion.js /
// js/server.js: each enters its module after installBroRoots and lifts the
// classes it defined. The sibling libraries' own APIs (bro.mesh, bro.lm,
// bro.stt, ...) are NOT here: installSiblingApis (host_natives.h) installs
// each exactly once per realm.
void installNetModule();
void installServerModule();
void installPhysicsModule();
void installTerrainModule();
void installClipmapModule();
void installTileWorldModule();
void installLightingModule();
void installGizmoModule();
void installAnimationModule();
void installSceneModule();
void installMotionModule();
void installImpostorModule();

inline Value makeFloat32Array(const float* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeFloat32Array(const std::vector<float>& vec) {
    return makeFloat32Array(vec.data(), vec.size());
}

inline Value makeUint32Array(const uint32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(uint32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeUint32Array(const std::vector<uint32_t>& vec) {
    return makeUint32Array(vec.data(), vec.size());
}

inline Value makeInt32Array(const int32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Int32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(int32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeInt32Array(const std::vector<int32_t>& vec) {
    return makeInt32Array(vec.data(), vec.size());
}

inline Value makeUint8Array(const uint8_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(data, count);
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeUint8Array(const std::vector<uint8_t>& vec) {
    return makeUint8Array(vec.data(), vec.size());
}

inline bool hostIsArray(Value v) {
    return v.isObject() && v.asObject<bronze::HeapObjectHeader>()->flags == bronze::HeapKind::Array;
}

inline bool readFloatVector(Value v, std::vector<float>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && (info.bytesPerElement == sizeof(float) || info.bytesPerElement == 0)) {
            const float* fp = reinterpret_cast<const float*>(info.data);
            out.assign(fp, fp + info.elementCount);
            return true;
        }
    }
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d =(!ev::isUndefined(e) && !ev::isObject(e)) ? ev::toDouble(e) : 0.0;
        out.push_back(static_cast<float>(d));
    }
    return true;
}

inline bool readU32Vector(Value v, std::vector<uint32_t>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && (info.bytesPerElement == sizeof(uint32_t) || info.bytesPerElement == 0)) {
            const uint32_t* up = reinterpret_cast<const uint32_t*>(info.data);
            out.assign(up, up + info.elementCount);
            return true;
        }
        if (info.data && info.bytesPerElement == sizeof(uint16_t)) {
            const uint16_t* up = reinterpret_cast<const uint16_t*>(info.data);
            out.clear();
            out.reserve(info.elementCount);
            for (uint32_t i = 0; i < info.elementCount; ++i) out.push_back(up[i]);
            return true;
        }
    }
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        uint32_t u =(!ev::isUndefined(e) && !ev::isObject(e)) ? satCast<uint32_t>(ev::toDouble(e)) : 0u;
        out.push_back(u);
    }
    return true;
}

physics::PhysicsWorld* unwrapPhysicsWorld(Value v);

// VideoEncoder / GifEncoder (host_video.cpp)
void installVideoGlobals();

// Math (host_math_classes.cpp / host_math_funcs.cpp)
Value makeBroMathValue();
void installMathGlobals();

// Text (host_text.cpp)
Value makeBroTextValue();

// bro.gpu (host_gpu.cpp): the runtime backend probe over brotensor/CPU fallback.
Value makeBroGpuValue();

// Menu (host_menu.cpp)
Value makeBroMenuValue();

// Steam (host_steam.cpp)
Value makeBroSteamValue();
void drainSteamEvents();
void cleanupSteamBindings();

// Media (host_media.cpp)
Value makeBroMediaValue();

// Stubs for unavailable / compiled-out subsystems (host_bro_root.cpp)
Value makeUnavailableNamespace(const std::string& name, const std::string& flag);

}  // namespace bro::bronze_host



