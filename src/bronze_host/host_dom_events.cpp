// Events between the engine's DOM and a bronze-compiled app: the canvas and
// document listeners a compiled program registers, the event data that reaches
// them, and the dispatch it can start itself.
//
// THE SEAM, and why there is no second dispatch here. bro's DOM already keeps
// TWO listener lists per target — the interpreted ones the QuickJS bindings
// store, and the native ones dom::Element::addEventListener holds — and
// js::dispatchDomEvent walks the event path ONCE, merging both on the shared
// registration sequence (dom/event_target.h). A compiled listener is just
// another native listener: it goes in the same list, fires in the same walk,
// in registration order against the page's own handlers, with the same capture
// / at-target / bubble phases and the same shadow retargeting. So a click()
// from the headless driver — which goes through hit testing and the real input
// pipeline — reaches a compiled handler for exactly the reason it reaches an
// interpreted one, and nothing in this file has an opinion about propagation.
//
// WHAT CROSSES, which is the other half of the design: nothing. The listener
// is handed a freshly built bronze object with COPIES of the fields the event
// kind carries — never a QuickJS value, never a pointer into either heap. The
// one thing that is shared rather than copied is the engine object behind
// `target`: a compiled listener that clicked its own canvas gets back the very
// canvas value it created, because identity is the whole use of a target.
//
// PROPAGATION, which needs a live event and therefore a lifetime. preventDefault
// / stopPropagation / stopImmediatePropagation must reach the dom::Event that
// dispatch is still walking with, so the event object carries a pointer to it —
// valid only while the listener is on the stack. It is cleared the instant the
// call returns, and a later call on a stored event object is a named TypeError
// rather than a write through a dangling pointer or a silent no-op.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

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
// Target identity
// ---------------------------------------------------------------------------

// The value a compiled listener sees for `target` / `currentTarget`. A canvas
// this layer created answers as itself, which is what an identity compare in
// the program is for. Anything else — the <html> element a document listener's
// currentTarget is, an element the page built — answers a small descriptor,
// because there is no host object to be identical to and inventing one would
// make two values for one element. ALLOCATES.
Value describeTarget(dom::Element* el) {
    if (!el) return ev::null();
    Value host = hostValueForElement(el);
    if (!ev::isUndefined(host)) return host;
    return hostElementValue(el);
}

// ---------------------------------------------------------------------------
// The event object
// ---------------------------------------------------------------------------

// Every field is read off the dom::Event and copied in. The kind branches are
// dynamic_casts for the same reason js/event_dispatch.cpp's populateJsEvent
// uses them: one event hierarchy, and the carrier decides what is there to
// copy. Order of registration is fixed source order, so the object's shape is
// the same every run — bronze's inline caches key off it. ALLOCATES heavily.
Value buildEventValue(dom::Event& e, const LiveEventPtr& live) {
    ObjectBuilder b;

    {
        Value type = ev::fromUtf8(e.type());
        b.set("type", type);
    }
    {
        Value target = describeTarget(e.target());
        b.set("target", target);
    }
    {
        Value cur = describeTarget(e.currentTarget());
        b.set("currentTarget", cur);
    }
    b.set("eventPhase", ev::fromDouble(e.eventPhase()));
    b.set("bubbles", ev::fromBool(e.bubbles()));
    b.set("cancelable", ev::fromBool(e.cancelable()));
    b.set("defaultPrevented", ev::fromBool(e.defaultPrevented()));
    b.set("isTrusted", ev::fromBool(e.isTrusted()));
    b.set("timeStamp", ev::fromDouble(e.timeStamp()));

    // CustomEvent first, so `detail` reads as the string payload for a custom
    // event and as the click count for a mouse event — the same two meanings
    // the web gives the name, on the same two event kinds.
    if (auto* custom = dynamic_cast<dom::CustomEvent*>(&e)) {
        Value detail = ev::fromUtf8(custom->detail());
        b.set("detail", detail);
    }

    if (auto* m = dynamic_cast<dom::MouseEvent*>(&e)) {
        b.set("clientX", ev::fromDouble(m->clientX()));
        b.set("clientY", ev::fromDouble(m->clientY()));
        b.set("pageX", ev::fromDouble(m->pageX()));
        b.set("pageY", ev::fromDouble(m->pageY()));
        b.set("screenX", ev::fromDouble(m->screenX()));
        b.set("screenY", ev::fromDouble(m->screenY()));
        b.set("offsetX", ev::fromDouble(m->offsetX()));
        b.set("offsetY", ev::fromDouble(m->offsetY()));
        b.set("movementX", ev::fromDouble(m->movementX()));
        b.set("movementY", ev::fromDouble(m->movementY()));
        b.set("button", ev::fromDouble(m->button()));
        b.set("buttons", ev::fromDouble(m->buttons()));
        b.set("detail", ev::fromDouble(m->detail()));
        b.set("ctrlKey", ev::fromBool(m->ctrlKey()));
        b.set("shiftKey", ev::fromBool(m->shiftKey()));
        b.set("altKey", ev::fromBool(m->altKey()));
        b.set("metaKey", ev::fromBool(m->metaKey()));
        // Pointer events ride on MouseEvent in this DOM (dom/event.h), and a
        // compiled handler duck-types them exactly as a JS one does.
        if (e.type().rfind("pointer", 0) == 0) {
            b.set("pointerId", ev::fromDouble(m->pointerId()));
            Value ptype = ev::fromUtf8(m->pointerType());
            b.set("pointerType", ptype);
            b.set("isPrimary", ev::fromBool(m->isPrimaryPointer()));
        }
        if (auto* w = dynamic_cast<dom::WheelEvent*>(&e)) {
            b.set("deltaX", ev::fromDouble(w->deltaX()));
            b.set("deltaY", ev::fromDouble(w->deltaY()));
            b.set("deltaZ", ev::fromDouble(w->deltaZ()));
            b.set("deltaMode", ev::fromDouble(w->deltaMode()));
        }
    }

    if (auto* drag = dynamic_cast<dom::DragEvent*>(&e)) {
        ObjectBuilder dt;
        dt.set("dropEffect", ev::fromUtf8("none"));
        dt.set("effectAllowed", ev::fromUtf8("all"));

        std::vector<std::string> typeList;
        if (!drag->files().empty()) typeList.push_back("Files");
        if (!drag->dataText().empty()) typeList.push_back("text/plain");
        Value typesArr = hostArrayOf(typeList.size(), [&typeList](size_t i) {
            return ev::fromUtf8(typeList[i]);
        });
        dt.set("types", typesArr);

        std::string dtText = drag->dataText();
        dt.def("getData", 1, [dtText](Value, std::span<const Value> a) {
            Value fV = argAt(a, 0);
            if (ev::isObject(fV) || ev::isUndefined(fV)) return ev::fromUtf8("");
            std::string fmt = ev::toUtf8(fV);
            for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fmt == "text" || fmt == "text/plain") return ev::fromUtf8(dtText);
            return ev::fromUtf8("");
        });
        dt.def("setData", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        dt.def("clearData", 1, [](Value, std::span<const Value>) { return ev::undefined(); });

        const auto& files = drag->files();
        Value filesArr = hostArrayOf(files.size(), [&files](size_t i) {
            const std::string& path = files[i];
            std::string name = std::filesystem::path(path).filename().string();
            ObjectBuilder f;
            f.set("name", ev::fromUtf8(name));
            f.set("path", ev::fromUtf8(path));
            f.set("size", ev::fromDouble(0));
            f.set("type", ev::fromUtf8(""));
            f.set("lastModified", ev::fromDouble(0));
            f.set("webkitRelativePath", ev::fromUtf8(""));
            return f.get();
        });
        dt.set("files", filesArr);

        Value itemsArr = hostArrayOf(files.size(), [&files](size_t i) {
            const std::string& path = files[i];
            std::string name = std::filesystem::path(path).filename().string();
            ObjectBuilder item;
            item.set("kind", ev::fromUtf8("file"));
            item.set("type", ev::fromUtf8(""));
            item.def("getAsFile", 0, [name, path](Value, std::span<const Value>) {
                ObjectBuilder f;
                f.set("name", ev::fromUtf8(name));
                f.set("path", ev::fromUtf8(path));
                f.set("size", ev::fromDouble(0));
                f.set("type", ev::fromUtf8(""));
                f.set("lastModified", ev::fromDouble(0));
                f.set("webkitRelativePath", ev::fromUtf8(""));
                return f.get();
            });
            item.def("webkitGetAsEntry", 0, [name, path](Value, std::span<const Value>) {
                ObjectBuilder entry;
                entry.set("isFile", ev::fromBool(true));
                entry.set("isDirectory", ev::fromBool(false));
                entry.set("name", ev::fromUtf8(name));
                entry.set("fullPath", ev::fromUtf8("/" + name));
                entry.def("file", 1, [name, path](Value, std::span<const Value> a) {
                    Value cb = argAt(a, 0);
                    if (ev::isFunction(cb)) {
                        ObjectBuilder f;
                        f.set("name", ev::fromUtf8(name));
                        f.set("path", ev::fromUtf8(path));
                        f.set("size", ev::fromDouble(0));
                        f.set("type", ev::fromUtf8(""));
                        f.set("lastModified", ev::fromDouble(0));
                        f.set("webkitRelativePath", ev::fromUtf8(""));
                        Value fileVal = f.get();
                        ev::call(cb, ev::undefined(), std::span<const Value>(&fileVal, 1));
                    }
                    return ev::undefined();
                });
                return entry.get();
            });
            return item.get();
        });
        dt.set("items", itemsArr);

        b.set("dataTransfer", dt.get());
    }

    if (auto* k = dynamic_cast<dom::KeyboardEvent*>(&e)) {
        Value key = ev::fromUtf8(k->key());
        b.set("key", key);
        Value code = ev::fromUtf8(k->code());
        b.set("code", code);
        b.set("repeat", ev::fromBool(k->repeat()));
        b.set("location", ev::fromDouble(k->location()));
        b.set("ctrlKey", ev::fromBool(k->ctrlKey()));
        b.set("shiftKey", ev::fromBool(k->shiftKey()));
        b.set("altKey", ev::fromBool(k->altKey()));
        b.set("metaKey", ev::fromBool(k->metaKey()));
    }

    // The three write-throughs. `live` is captured by value: the closures
    // outlive this function (they live on the event object), and the box is
    // what tells them whether the event still exists.
    b.def("preventDefault", 0, [live](Value, std::span<const Value>) {
        if (!live->ev) return staleEventThrow("preventDefault");
        live->ev->preventDefault();
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

    return b.get();
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
};

std::vector<ElementListener>& registrations() {
    static auto* list = new std::vector<ElementListener>();
    return *list;
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
// Descriptors for a dispatch the program starts
// ---------------------------------------------------------------------------

struct EventSpec {
    std::string type;
    bool bubbles = true;
    bool cancelable = true;
    bool hasDetail = false;
    std::string detail;
    std::string key;
    std::string code;
};

// Reads `{type, bubbles, cancelable, detail, key, code}`. False leaves a pending
// TypeError naming what was wrong: a dispatch with no type is a program bug,
// and a silently dropped one would look exactly like a listener that never
// ran. `bubbles`/`cancelable` default to true — a custom event between the two
// worlds is meant to be heard by a document listener, and a non-bubbling
// default would make the common case look broken.
bool readEventSpec(Value descV, const char* what, EventSpec& out) {
    if (!ev::isObject(descV)) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: expects an event object, e.g. "
                           "{ type: 'app:ping', detail: 'text' }");
        return false;
    }
    ev::Persistent desc(descV);
    Value typeV = ev::getProperty(desc.get(), "type");
    if (ev::isObject(typeV) || ev::isUndefined(typeV) || ev::isNull(typeV)) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: the event object needs a string `type`");
        return false;
    }
    out.type = ev::toUtf8(typeV);
    if (out.type.empty()) {
        ev::throwTypeError(std::string(what) +
                           ".dispatchEvent: `type` must not be empty");
        return false;
    }

    Value bubblesV = ev::getProperty(desc.get(), "bubbles");
    out.bubbles = ev::isUndefined(bubblesV) ? true : ev::toBool(bubblesV);
    Value cancelableV = ev::getProperty(desc.get(), "cancelable");
    out.cancelable = ev::isUndefined(cancelableV) ? true : ev::toBool(cancelableV);

    // Only a string detail crosses — dom::CustomEvent says why. A detail of
    // any other type is refused rather than stringified: "[object Object]"
    // arriving on the other side is worse than being told it cannot go.
    Value detailV = ev::getProperty(desc.get(), "detail");
    if (!ev::isUndefined(detailV) && !ev::isNull(detailV)) {
        if (ev::isObject(detailV)) {
            ev::throwTypeError(
                std::string(what) +
                ".dispatchEvent: `detail` must be a string — an object cannot cross "
                "between the compiled and interpreted heaps (src/bronze_host/README.md)");
            return false;
        }
        out.hasDetail = true;
        out.detail = ev::toUtf8(detailV);
    }

    Value keyV = ev::getProperty(desc.get(), "key");
    if (!ev::isUndefined(keyV) && !ev::isNull(keyV) && !ev::isObject(keyV)) {
        out.key = ev::toUtf8(keyV);
    }
    Value codeV = ev::getProperty(desc.get(), "code");
    if (!ev::isUndefined(codeV) && !ev::isNull(codeV) && !ev::isObject(codeV)) {
        out.code = ev::toUtf8(codeV);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The public seam
// ---------------------------------------------------------------------------

void callBronzeListener(const ev::Persistent& fn, const ev::Persistent& thisObj,
                        dom::Event& evt, const char* origin) {
    auto live = std::make_shared<LiveEvent>();
    live->ev = &evt;
    // The event object is rooted for the call: buildEventValue's own
    // allocations are done, but ev::call allocates to root the arguments.
    ev::Persistent evtObj(buildEventValue(evt, live));
    Value arg = evtObj.get();
    ev::CallResult r = ev::call(fn.get(), thisObj.get(), std::span<const Value>(&arg, 1));
    // Before the report, and before anything else can run: from here on the
    // dom::Event may be destroyed at any point and the object must not reach
    // it. A listener that stashed the object gets the named refusal instead.
    live->ev = nullptr;
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

        // A repeat (type, listener) pair is a no-op on the web; the engine's
        // native list has no such rule of its own, so it is applied here.
        for (const ElementListener& r : registrations()) {
            if (r.el == el && r.type == type &&
                ev::toBits(r.fn.get()) == ev::toBits(fnP.get())) {
                return ev::undefined();
            }
        }

        std::string origin = who + " " + type + " listener";
        dom::ListenerHandle handle = el->addEventListener(
            type,
            [fnP, self, origin](dom::Event& evt) {
                callBronzeListener(fnP, self, evt, origin.c_str());
            },
            opts);
        if (!handle) {
            return ev::throwError(who + ".addEventListener: the engine refused the "
                                        "registration");
        }
        registrations().push_back({el, std::move(type), fnP, handle});
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
        auto& list = registrations();
        for (auto it = list.begin(); it != list.end(); ++it) {
            // Identity by a compare of two CURRENT addresses with no
            // allocation between them — the one moment raw bits are a valid
            // identity for heap values.
            if (it->el != el || it->type != type) continue;
            if (ev::toBits(it->fn.get()) != ev::toBits(fn)) continue;
            if (el) el->removeEventListener(it->handle);
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
    EventSpec spec;
    if (!readEventSpec(desc, what, spec)) return ev::undefined();
    engine::Engine* engine = hostEngine();
    dom::Element* el = source();
    if (!engine || !el) {
        return ev::throwError(std::string(what) +
                              ".dispatchEvent: no element to dispatch at");
    }
    if (spec.type == "keydown" || spec.type == "keyup") {
        dom::KeyboardEvent k(spec.type, spec.bubbles, spec.cancelable);
        k.setKey(spec.key);
        k.setCode(spec.code.empty() ? spec.key : spec.code);
        engine->dispatchElementEvent(el, k);
        return ev::fromBool(!k.defaultPrevented());
    }
    dom::CustomEvent custom(spec.type, spec.bubbles, spec.cancelable);
    dom::Event plain(spec.type, spec.bubbles, spec.cancelable);
    dom::Event& evt = spec.hasDetail ? static_cast<dom::Event&>(custom) : plain;
    if (spec.hasDetail) custom.setDetail(spec.detail);
    // Runs interpreted listeners and compiled ones both, re-entering this
    // layer for the compiled ones. Single-threaded and re-entrant by
    // construction: nothing here holds a bare Value across the call.
    engine->dispatchElementEvent(el, evt);
    return ev::fromBool(!evt.defaultPrevented());
}

Value hostDispatchToWindow(Value desc) {
    EventSpec spec;
    if (!readEventSpec(desc, "window", spec)) return ev::undefined();
    engine::Engine* engine = hostEngine();
    if (!engine) return ev::throwError("window.dispatchEvent: no engine");
    if (spec.type == "keydown" || spec.type == "keyup") {
        dom::KeyboardEvent k(spec.type, spec.bubbles, spec.cancelable);
        k.setKey(spec.key);
        k.setCode(spec.code.empty() ? spec.key : spec.code);
        engine->dispatchWindowEvent(k);
        return ev::fromBool(!k.defaultPrevented());
    }
    dom::CustomEvent custom(spec.type, spec.bubbles, spec.cancelable);
    dom::Event plain(spec.type, spec.bubbles, spec.cancelable);
    dom::Event& evt = spec.hasDetail ? static_cast<dom::Event&>(custom) : plain;
    if (spec.hasDetail) custom.setDetail(spec.detail);
    engine->dispatchWindowEvent(evt);
    return ev::fromBool(!evt.defaultPrevented());
}

}  // namespace bro::bronze_host
