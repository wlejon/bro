// Events between the engine's DOM and a bronze-compiled app: the canvas and
// document listeners a compiled program registers, the event data that reaches
// them, and the dispatch it can start itself.
//
// THE SEAM, and why there is no second dispatch here. The DOM listener
// registration is held natively on dom::Element, and dom::dispatchDomEvent
// walks the event path with capture / at-target / bubble phases and shadow
// retargeting. A compiled listener is a native listener: it fires in the same
// walk, in registration order, with the same phases. So input dispatch reaches
// a compiled handler naturally.
//
// WHAT CROSSES: The listener is handed a freshly built bronze object with
// copies of the fields the event kind carries. The engine object behind `target`
// is resolved to its wrapper because identity is the whole use of a target.
//
// PROPAGATION, which needs a live event and therefore a lifetime. preventDefault
// / stopPropagation / stopImmediatePropagation must reach the dom::Event that
// dispatch is still walking with, so the event object carries a pointer to it —
// valid only while the listener is on the stack. It is cleared the instant the
// call returns, and a later call on a stored event object is a named TypeError
// rather than a write through a dangling pointer or a silent no-op.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_anchor_download.h"
#include "bronze_host/host_event_spec.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_touch.h"
#include "bronze_host/host_dom_events_types.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "dom/event_dispatch.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::bronze_host {

struct DragSessionState {
    std::unordered_map<std::string, std::string> data;
    std::string effectAllowed = "all";
    std::string dropEffect = "none";
};
static DragSessionState g_dragSession;

// ---------------------------------------------------------------------------
// Target identity
// ---------------------------------------------------------------------------

Value describeTarget(dom::Element* el) {
    if (!el) return ev::null();
    Value host = hostValueForElement(el);
    if (!ev::isUndefined(host)) return host;
    return hostElementValue(el);
}

namespace {

// The window is an event target without an Element behind it; dom::Event
// flags it, and this answers the realm's `window` object for it.
Value windowObjectValue() {
    ev::GlobalValue w = ev::globalValue("window");
    return w.found ? w.value : ev::null();
}

Value eventTargetValue(const dom::Event& e) {
    return e.targetIsWindow() ? windowObjectValue() : describeTarget(e.target());
}

Value eventCurrentTargetValue(const dom::Event& e) {
    return e.currentTargetIsWindow() ? windowObjectValue() : describeTarget(e.currentTarget());
}

// ---------------------------------------------------------------------------
// The live event, and the window it is live in
// ---------------------------------------------------------------------------

// One box per listener invocation, shared with the three propagation methods
// on the event object handed over. `ev` is the dispatching dom::Event while
// the listener runs and nullptr the moment it returns — so an event object the
// program squirrelled away is inert rather than dangerous.
struct LiveEvent {
    dom::Event* ev = nullptr;
};
using LiveEventPtr = std::shared_ptr<LiveEvent>;

Value staleEventThrow(const char* method) {
    return ev::throwTypeError(
        std::string("event.") + method +
        ": the event is no longer being dispatched. The event object handed to a "
        "listener is only live for the duration of that listener call.");
}

// ---------------------------------------------------------------------------
// The event object
// ---------------------------------------------------------------------------

// Every field is read off the dom::Event and copied in. The kind branches are
// dynamic_casts for the same reason js/event_dispatch.cpp's populateJsEvent
// uses them: one event hierarchy, and the carrier decides what is there to
// copy. Order of registration is fixed source order, so the object's shape is
// the same every run — bronze's inline caches key off it. ALLOCATES heavily.
// The three write-throughs. `live` is captured by value: the closures outlive
// the call that installs them (they live on the event object), and the box is
// what tells them whether the dom::Event still exists.
void installEventPropagationMethods(ObjectBuilder& b, const LiveEventPtr& live) {
    b.def("preventDefault", 0, [live](Value self_, std::span<const Value>) {
        if (!live->ev) return staleEventThrow("preventDefault");
        live->ev->preventDefault();
        ev::setProperty(self_, "defaultPrevented", ev::fromBool(true));
        return ev::undefined();
    });
    b.def("stopPropagation", 0, [live](Value, std::span<const Value>) {
        if (!live->ev) return staleEventThrow("stopPropagation");
        live->ev->stopPropagation();
        return ev::undefined();
    });
    b.def("stopImmediatePropagation", 0, [live](Value, std::span<const Value>) {
        if (!live->ev) return staleEventThrow("stopImmediatePropagation");
        live->ev->stopImmediatePropagation();
        return ev::undefined();
    });
}

// ---------------------------------------------------------------------------
// The event object a dispatchEvent() caller supplied
// ---------------------------------------------------------------------------
//
// A stack, not a slot: a listener may dispatch another event from inside this
// one, and each level has its own object. Entries are pushed for the duration
// of one dom::dispatchDomEvent walk and matched by the dom::Event's address,
// which is unique for as long as the walk owns it.
struct ProvidedEvent {
    dom::Event* ev = nullptr;
    ev::Persistent obj;
};

std::vector<ProvidedEvent>& providedEvents() {
    static auto* list = new std::vector<ProvidedEvent>();
    return *list;
}

Value providedEventObjectFor(dom::Event& e) {
    auto& list = providedEvents();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (it->ev == &e) return it->obj.get();
    }
    return ev::undefined();
}

// Pushes on construction, pops on scope exit — including when a listener
// throws its way out, which is why this is a guard and not two calls.
struct ProvidedEventScope {
    explicit ProvidedEventScope(dom::Event& e, Value obj) {
        providedEvents().push_back({&e, ev::Persistent(obj)});
    }
    ~ProvidedEventScope() { providedEvents().pop_back(); }
};

Value buildEventValue(dom::Event& e, const LiveEventPtr& live) {
    Value baseObj = ev::undefined();
    if (dynamic_cast<dom::TouchEvent*>(&e)) {
        baseObj = g_touchEventClass.make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::GestureEvent*>(&e)) {
        baseObj = g_gestureEventClass.make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::WheelEvent*>(&e)) {
        baseObj = wheelEventHostClass().make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::MouseEvent*>(&e)) {
        baseObj = mouseEventHostClass().make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::KeyboardEvent*>(&e)) {
        baseObj = keyboardEventHostClass().make(nullptr, [](void*) {});
    } else if (e.type() == "focus" || e.type() == "blur" || e.type() == "focusin" || e.type() == "focusout") {
        baseObj = focusEventHostClass().make(nullptr, [](void*) {});
    } else if (e.type() == "gamepadconnected" || e.type() == "gamepaddisconnected") {
        baseObj = gamepadEventHostClass().make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::CustomEvent*>(&e)) {
        baseObj = customEventHostClass().make(nullptr, [](void*) {});
    }
    ObjectBuilder b(ev::isUndefined(baseObj) ? ev::createObject() : baseObj);

    {
        Value type = ev::fromUtf8(e.type());
        b.set("type", type);
    }
    {
        Value target = eventTargetValue(e);
        b.set("target", target);
    }
    {
        Value cur = eventCurrentTargetValue(e);
        b.set("currentTarget", cur);
    }
    b.set("eventPhase", ev::fromDouble(e.eventPhase()));
    b.set("bubbles", ev::fromBool(e.bubbles()));
    b.set("cancelable", ev::fromBool(e.cancelable()));
    b.set("composed", ev::fromBool(true));
    b.set("defaultPrevented", ev::fromBool(e.defaultPrevented()));
    b.set("isTrusted", ev::fromBool(e.isTrusted()));
    b.set("timeStamp", ev::fromDouble(e.timeStamp()));

    // CustomEvent first, so `detail` reads as the string payload for a custom
    // event and as the click count for a mouse event — the same two meanings
    // the web gives the name, on the same two event kinds.
    if (auto* custom = dynamic_cast<dom::CustomEvent*>(&e)) {
        const std::string& det = custom->detail();
        Value detail = ev::null();
        if (!det.empty()) {
            if (det.front() == '{' || det.front() == '[') {
                ev::GlobalValue g = ev::globalValue("JSON");
                if (g.found && ev::isObject(g.value)) {
                    Value parseFn = ev::getProperty(g.value, "parse");
                    if (ev::isFunction(parseFn)) {
                        Value sVal = ev::fromUtf8(det);
                        ev::CallResult res = ev::call(parseFn, g.value, std::span<const Value>(&sVal, 1));
                        if (!res.thrown && !ev::isUndefined(res.value)) {
                            detail = res.value;
                        }
                    }
                }
            }
            if (ev::isNull(detail) || ev::isUndefined(detail)) {
                detail = ev::fromUtf8(det);
            }
        }
        b.set("detail", detail);
    }

    if (e.type() == "gamepadconnected" || e.type() == "gamepaddisconnected") {
        auto* custom = dynamic_cast<dom::CustomEvent*>(&e);
        int idx = -1;
        if (custom && !custom->detail().empty()) {
            try { idx = std::stoi(custom->detail()); } catch (...) {}
        }
        auto* eng = hostEngine();
        if (eng && idx >= 0 && idx < static_cast<int>(eng->gamepads().size())) {
            b.set("gamepad", buildGamepadSnapshot(eng->gamepads()[idx]));
        } else {
            b.set("gamepad", ev::null());
        }
    }

    populateMouseEvent(b, e);

    if (auto* drag = dynamic_cast<dom::DragEvent*>(&e)) {
        if (drag->type() == "dragstart") {
            g_dragSession.data.clear();
            g_dragSession.effectAllowed = "all";
            g_dragSession.dropEffect = "none";
        }

        ObjectBuilder dt;

        dt.accessor(
            "effectAllowed",
            [](Value, std::span<const Value>) {
                return ev::fromUtf8(g_dragSession.effectAllowed);
            },
            [](Value, std::span<const Value> a) {
                if (!a.empty()) g_dragSession.effectAllowed = ev::toUtf8(a[0]);
                return ev::undefined();
            });

        dt.accessor(
            "dropEffect",
            [](Value, std::span<const Value>) {
                return ev::fromUtf8(g_dragSession.dropEffect);
            },
            [](Value, std::span<const Value> a) {
                if (!a.empty()) g_dragSession.dropEffect = ev::toUtf8(a[0]);
                return ev::undefined();
            });

        dt.accessor(
            "types",
            [drag](Value, std::span<const Value>) {
                std::vector<std::string> typeList;
                if (!drag->files().empty()) typeList.push_back("Files");
                for (const auto& [k, v] : g_dragSession.data) {
                    if (std::find(typeList.begin(), typeList.end(), k) == typeList.end()) {
                        typeList.push_back(k);
                    }
                }
                if (typeList.empty() && !drag->dataText().empty()) {
                    typeList.push_back("text/plain");
                }
                return hostArrayOf(typeList.size(), [&typeList](size_t i) {
                    return ev::fromUtf8(typeList[i]);
                });
            },
            nullptr);

        std::string dtText = drag->dataText();
        dt.def("getData", 1, [dtText](Value, std::span<const Value> a) {
            Value fV = argAt(a, 0);
            if (ev::isObject(fV) || ev::isUndefined(fV)) return ev::fromUtf8("");
            std::string fmt = ev::toUtf8(fV);
            for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fmt == "text") fmt = "text/plain";
            auto it = g_dragSession.data.find(fmt);
            if (it != g_dragSession.data.end()) return ev::fromUtf8(it->second);
            if (fmt == "text/plain" && !dtText.empty()) return ev::fromUtf8(dtText);
            return ev::fromUtf8("");
        });

        dt.def("setData", 2, [](Value, std::span<const Value> a) {
            if (a.size() < 2) return ev::undefined();
            std::string fmt = ev::toUtf8(a[0]);
            for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fmt == "text") fmt = "text/plain";
            std::string val = ev::toUtf8(a[1]);
            g_dragSession.data[fmt] = val;
            return ev::undefined();
        });

        dt.def("clearData", 1, [](Value, std::span<const Value> a) {
            if (a.empty() || ev::isUndefined(a[0])) {
                g_dragSession.data.clear();
            } else {
                std::string fmt = ev::toUtf8(a[0]);
                for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (fmt == "text") fmt = "text/plain";
                g_dragSession.data.erase(fmt);
            }
            return ev::undefined();
        });

        // A dropped file is a REAL File — bytes and all (host_file_path.cpp);
        // one the disk cannot supply is the {name, path} descriptor, so the
        // list is as long as the drop was. `items` carries the same Files
        // behind `getAsFile`, and its `webkitGetAsEntry().file(cb)` calls
        // back on the FRAME SEAM rather than synchronously, because that
        // call is asynchronous on the web and code written against it counts
        // on it — the three.js editor's `getFilesFromItemList` increments
        // its "handled" counter in the callback and its "total" on the next
        // line, so a synchronous callback makes it decide the batch is
        // unfinished and drop every dropped file.
        const auto& files = drag->files();
        Value filesArr = hostArrayOf(files.size(), [&files](size_t i) {
            return makeFileOrDescriptorFromPath(files[i]);
        });
        dt.set("files", filesArr);

        Value itemsArr = hostArrayOf(files.size(), [&files](size_t i) {
            const std::string& path = files[i];
            const std::string name = std::filesystem::path(path).filename().string();
            ObjectBuilder item;
            item.set("kind", ev::fromUtf8("file"));
            item.set("type", ev::fromUtf8(""));
            item.def("getAsFile", 0, [path](Value, std::span<const Value>) {
                Value f = makeFileFromPath(path);
                return ev::isUndefined(f) ? ev::null() : f;
            });
            item.def("webkitGetAsEntry", 0, [name, path](Value, std::span<const Value>) {
                ObjectBuilder entry;
                entry.set("isFile", ev::fromBool(true));
                entry.set("isDirectory", ev::fromBool(false));
                entry.set("name", ev::fromUtf8(name));
                entry.set("fullPath", ev::fromUtf8("/" + name));
                entry.def("file", 1, [path](Value, std::span<const Value> a) {
                    Value cb = argAt(a, 0);
                    if (!ev::isFunction(cb)) {
                        return ev::throwTypeError("FileSystemFileEntry.file: the argument must be a function");
                    }
                    auto held = std::make_shared<ev::Persistent>(cb);
                    postHostTask([held, path]() {
                        Value fileVal = makeFileFromPath(path);
                        if (ev::isUndefined(fileVal)) return;
                        ev::CallResult r = ev::call(held->get(), ev::undefined(),
                                                    std::span<const Value>(&fileVal, 1));
                        if (r.thrown) reportBronzeError("FileSystemFileEntry.file", r.value);
                    });
                    return ev::undefined();
                });
                return entry.get();
            });
            return item.get();
        });
        dt.set("items", itemsArr);

        b.set("dataTransfer", ev::setPrototype(dt.get(), dataTransferHostClass().prototype()));

        if (drag->type() == "dragend") {
            g_dragSession.data.clear();
            g_dragSession.effectAllowed = "all";
            g_dragSession.dropEffect = "none";
        }
    }

    populateKeyboardEvent(b, e);

    if (auto* te = dynamic_cast<dom::TouchEvent*>(&e)) {
        b.set("touches", makeTouchListValue(te->touches()));
        b.set("targetTouches", makeTouchListValue(te->targetTouches()));
        b.set("changedTouches", makeTouchListValue(te->changedTouches()));
        b.set("ctrlKey", ev::fromBool(te->ctrlKey()));
        b.set("shiftKey", ev::fromBool(te->shiftKey()));
        b.set("altKey", ev::fromBool(te->altKey()));
        b.set("metaKey", ev::fromBool(te->metaKey()));
    }

    if (auto* ge = dynamic_cast<dom::GestureEvent*>(&e)) {
        b.set("scale", ev::fromDouble(ge->scale()));
        b.set("rotation", ev::fromDouble(ge->rotation()));
        b.set("clientX", ev::fromDouble(ge->clientX()));
        b.set("clientY", ev::fromDouble(ge->clientY()));
    }

    populateInputAndFormEvents(b, e);
    populateClipboardEvent(b, e);

    installEventPropagationMethods(b, live);

    return b.get();
}

// The object a program handed to dispatchEvent, made usable as THE event.
//
// Only the per-dispatch facts are written: which element the event is at now,
// which phase, whether the default has been prevented, and the three methods
// that reach the live dom::Event. Everything else — `type`, `bubbles`, and
// above all `detail` — is left exactly as the program wrote it, which is the
// whole point. Copying the descriptor into a fresh object (what the port did)
// loses object identity, and a `detail` that is not a string loses its
// contents as well: it went out through JSON.stringify and came back as text,
// so a function, a DOM node or a class instance in a CustomEvent's payload
// arrived as `[object Object]` or vanished.
Value decorateProvidedEventValue(Value provided, dom::Event& e, const LiveEventPtr& live) {
    ObjectBuilder b(provided);
    {
        Value target = eventTargetValue(e);
        b.set("target", target);
    }
    {
        Value cur = eventCurrentTargetValue(e);
        b.set("currentTarget", cur);
    }
    b.set("eventPhase", ev::fromDouble(e.eventPhase()));
    b.set("defaultPrevented", ev::fromBool(e.defaultPrevented()));
    b.set("isTrusted", ev::fromBool(e.isTrusted()));
    installEventPropagationMethods(b, live);
    return b.get();
}

// The caller's own object once dispatch returns: it keeps its target (even
// when no listener ran to see it), and its currentTarget and phase are
// cleared, which is what the program reads off it after dispatchEvent().
void finishProvidedEventValue(Value provided, const dom::Event& e) {
    ObjectBuilder b(provided);
    {
        Value target = eventTargetValue(e);
        b.set("target", target);
    }
    b.set("currentTarget", ev::null());
    b.set("eventPhase", ev::fromDouble(0));
    b.set("defaultPrevented", ev::fromBool(e.defaultPrevented()));
}

// ---------------------------------------------------------------------------
// Registrations
// ---------------------------------------------------------------------------

// What removeEventListener needs to find the engine handle again, given the
// (type, function) pair the program passes. Process-lived and never freed, the
// same convention the rest of this layer's state follows — and the elements in
// it are the ones the Document owns for its whole life (a host canvas, the
// document element), so a raw pointer here cannot outlive its target.
struct ElementListener {
    dom::Element* el = nullptr;
    std::string type;
    ev::Persistent fn;
    dom::ListenerHandle handle;
    bool capture = false;
    bool once = false;
};

std::vector<ElementListener>& registrations() {
    static auto* list = new std::vector<ElementListener>();
    return *list;
}

} // namespace

void clearElementListeners(dom::Element* el) {
    if (!el) return;
    auto& list = registrations();
    for (auto it = list.begin(); it != list.end(); ) {
        if (it->el == el) {
            it->fn.set(ev::undefined());
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

void clearElementListenersForDocument(dom::Document* doc) {
    if (!doc) return;
    auto& list = registrations();
    for (auto it = list.begin(); it != list.end(); ) {
        if (it->el && it->el->document() == doc) {
            it->fn.set(ev::undefined());
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

void clearAllElementListeners() {
    auto& list = registrations();
    for (auto& r : list) {
        r.fn.set(ev::undefined());
    }
    list.clear();
}

// addEventListener's third argument: `true` for capture, or an options object.
// Anything else (absent, false, a number) is the default — the same shape the
// web accepts, minus `passive`, which this DOM does not model anywhere.
dom::ListenerOptions readOptions(Value optV) {
    dom::ListenerOptions opts;
    if (ev::isObject(optV)) {
        ev::Persistent root(optV);
        Value capture = ev::getProperty(root.get(), "capture");
        opts.capture = ev::toBool(capture);
        Value once = ev::getProperty(root.get(), "once");
        opts.once = ev::toBool(once);
        return opts;
    }
    opts.capture = ev::toBool(optV);
    return opts;
}

// ---------------------------------------------------------------------------
// The public seam
// ---------------------------------------------------------------------------

void callBronzeListener(const ev::Persistent& fn, const ev::Persistent& thisObj,
                        dom::Event& evt, const char* origin,
                        dom::Document* docOverride) {
    dom::Document* targetDoc = docOverride;
    if (!targetDoc) {
        if (evt.target()) targetDoc = evt.target()->document();
        else if (evt.currentTarget()) targetDoc = evt.currentTarget()->document();
    }

    dom::Document* prevDoc = currentHostDocument();
    bool swapDoc = (targetDoc && targetDoc != prevDoc);

    ev::GlobalValue docG = ev::globalValue("document");
    ev::GlobalValue gt = ev::globalValue("globalThis");
    Value prevDocVal = docG.found ? docG.value : ev::null();

    if (swapDoc) {
        enterRealmScope(scopeIdForDocument(targetDoc));
        setCurrentHostDocument(targetDoc);
        Value subDocVal = hostDocumentValue(targetDoc);
        ev::registerGlobal("document", subDocVal);
        if (gt.found && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "document", subDocVal);
        }
    }

    auto live = std::make_shared<LiveEvent>();
    live->ev = &evt;
    // The event object is rooted for the call: buildEventValue's own
    // allocations are done, but ev::call allocates to root the arguments.
    //
    // When this dispatch started from `el.dispatchEvent(obj)`, THAT object is
    // the event — same identity for every listener on the path, and with the
    // program's own `detail` untouched.
    Value provided = providedEventObjectFor(evt);
    ev::Persistent evtObj(ev::isObject(provided)
                              ? decorateProvidedEventValue(provided, evt, live)
                              : buildEventValue(evt, live));
    Value arg = evtObj.get();
    ev::CallResult r = ev::call(fn.get(), thisObj.get(), std::span<const Value>(&arg, 1));
    // Before the report, and before anything else can run: from here on the
    // dom::Event may be destroyed at any point and the object must not reach
    // it. A listener that stashed the object gets the named refusal instead.
    live->ev = nullptr;

    if (swapDoc) {
        if (!ev::isNull(prevDocVal)) {
            ev::registerGlobal("document", prevDocVal);
            if (gt.found && ev::isObject(gt.value)) {
                ev::setProperty(gt.value, "document", prevDocVal);
            }
        }
        setCurrentHostDocument(prevDoc);
        exitRealmScope();
    }

    // Report and keep going — one broken listener must not silence the ones
    // registered after it, which is what the rAF and window paths do too.
    if (r.thrown) reportBronzeError(origin, r.value);
}

namespace {

// The receiver's element, for the shared form of the trio below. An
// ElementSource captures ONE element, so a method built from it can only live
// on that element's own object — which is why these three were the last
// per-instance members an element had. Reading the receiver instead is the
// same answer without the capture, so ONE function object serves every
// element and two elements' `addEventListener` are the same value, the way
// they are on the web and the way the other fifty-eight members here already
// are.
dom::Element* elementOfReceiver(Value self) {
    HostNodeState* st = hostNodeStateOfValue(self);
    return st ? st->el : nullptr;
}

// `source` may be null, and that is the prototype form: resolve the element
// from the receiver, and name it in diagnostics by its own tag.
dom::Element* targetElement(const ElementSource& source, Value thisValue) {
    return source ? source() : elementOfReceiver(thisValue);
}

std::string targetName(const ElementSource& source, const std::string& name,
                       dom::Element* el) {
    if (source || !el) return name;
    return el->tagName();
}

}  // namespace

void installElementEventTarget(ObjectBuilder& b, ElementSource source,
                               const char* what) {
    std::string name = what;

    b.def("addEventListener", 3, [source, name](Value thisValue,
                                                std::span<const Value> a) {
        // thisValue is current at entry only; root it before anything below
        // allocates (embed.h's NativeFn contract).
        ev::Persistent self(thisValue);
        dom::Element* el = targetElement(source, thisValue);
        const std::string who = targetName(source, name, el);
        Value typeV = argAt(a, 0);
        Value fn = argAt(a, 1);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) {
            return ev::throwTypeError(who + ".addEventListener: type must be a string");
        }
        if (!ev::isFunction(fn)) {
            return ev::throwTypeError(who +
                                      ".addEventListener: listener must be a function");
        }
        ev::Persistent fnP(fn);
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        std::string type = ev::toUtf8(typeV);

        if (!el) {
            // The registration says so rather than vanishing: a listener the
            // program believes is attached, on a target that does not exist,
            // is the failure this whole file is here to end.
            return ev::throwError(who +
                                  ".addEventListener: the element does not exist yet");
        }

        // A repeat (type, listener, capture) triple is a no-op on the web; the engine's
        // native list has no such rule of its own, so it is applied here.
        for (const ElementListener& r : registrations()) {
            if (r.el == el && r.type == type && r.capture == opts.capture &&
                ev::toBits(r.fn.get()) == ev::toBits(fnP.get())) {
                return ev::undefined();
            }
        }

        std::string origin = who + " " + type + " listener";
        bool isOnce = opts.once;
        dom::ListenerHandle handle = el->addEventListener(
            type,
            [fnP, self, origin, el, type, isOnce](dom::Event& evt) {
                if (isOnce) {
                    if (el) el->removeJsListener(type);
                    auto& list = registrations();
                    for (auto it = list.begin(); it != list.end(); ++it) {
                        if (it->el == el && it->type == type &&
                            ev::toBits(it->fn.get()) == ev::toBits(fnP.get())) {
                            list.erase(it);
                            break;
                        }
                    }
                }
                callBronzeListener(fnP, self, evt, origin.c_str());
            },
            opts);
        if (!handle) {
            return ev::throwError(who + ".addEventListener: the engine refused the "
                                        "registration");
        }
        el->addJsListener(type);
        registrations().push_back({el, std::move(type), fnP, handle, opts.capture, opts.once});
        return ev::undefined();
    });

    b.def("removeEventListener", 3, [source, name](Value thisValue,
                                                   std::span<const Value> a) {
        dom::Element* el = targetElement(source, thisValue);
        Value typeV = argAt(a, 0);
        Value fn = argAt(a, 1);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) {
            return ev::throwTypeError(targetName(source, name, el) +
                                      ".removeEventListener: type must be a string");
        }
        std::string type = ev::toUtf8(typeV);
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        auto& list = registrations();
        for (auto it = list.begin(); it != list.end(); ++it) {
            // Identity by a compare of two CURRENT addresses with no
            // allocation between them — the one moment raw bits are a valid
            // identity for heap values.
            if (it->el != el || it->type != type) continue;
            if (it->capture != opts.capture) continue;
            if (ev::toBits(it->fn.get()) != ev::toBits(fn)) continue;
            if (el) {
                el->removeEventListener(it->handle);
                el->removeJsListener(type);
            }
            list.erase(it);
            break;
        }
        return ev::undefined();
    });

    b.def("dispatchEvent", 1, [source, name](Value thisValue,
                                             std::span<const Value> a) {
        dom::Element* el = targetElement(source, thisValue);
        const std::string who = targetName(source, name, el);
        return hostDispatchToElement([el]() { return el; }, who.c_str(), argAt(a, 0));
    });
}

Value hostDispatchToElement(ElementSource source, const char* what, Value desc) {
    // Rooted first: readEventSpec reads properties, which allocates.
    ev::Persistent descRoot(desc);
    EventSpec spec;
    if (!readEventSpec(descRoot.get(), what, spec)) return ev::undefined();
    engine::Engine* engine = hostEngine();
    dom::Element* el = source();
    if (!engine || !el) {
        return ev::throwError(std::string(what) +
                              ".dispatchEvent: no element to dispatch at");
    }
    const bool notPrevented = dispatchEventSpec(spec, [engine, el, &descRoot](dom::Event& evt) {
        // Listeners on the path receive the caller's own object, not a copy of
        // its fields — see decorateProvidedEventValue.
        {
            ProvidedEventScope scope(evt, descRoot.get());
            engine->dispatchElementEvent(el, evt);
        }
        finishProvidedEventValue(descRoot.get(), evt);
    });
    if (spec.type == "click" && notPrevented) {
        runAnchorDownload(el);
    }
    return ev::fromBool(notPrevented);
}

// The window of the realm the caller is running in: a secondary window's or an
// iframe's own document when one is current, the main document otherwise.
// Every realm shares one `window` object, so dispatching at "the" window has
// to mean this one — the main document's listeners are not a secondary
// window's.
Value hostDispatchToWindow(Value desc) {
    engine::Engine* engine = hostEngine();
    if (!engine) return ev::throwError("window.dispatchEvent: no engine");
    dom::Document* doc = currentHostDocument();
    return hostDispatchToWindowOf(doc ? doc : engine->document(), desc);
}

Value hostDispatchToWindowOf(dom::Document* doc, Value desc) {
    // Rooted first: readEventSpec reads properties, which allocates.
    ev::Persistent descRoot(desc);
    EventSpec spec;
    if (!readEventSpec(descRoot.get(), "window", spec)) return ev::undefined();
    if (!doc) return ev::throwError("window.dispatchEvent: no document");
    return ev::fromBool(dispatchEventSpec(spec, [doc, &descRoot](dom::Event& evt) {
        {
            ProvidedEventScope scope(evt, descRoot.get());
            dom::dispatchWindowEvent(doc, evt);
        }
        finishProvidedEventValue(descRoot.get(), evt);
    }));
}

}  // namespace bro::bronze_host
