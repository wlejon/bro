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

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_anchor_download.h"
#include "bronze_host/host_event_spec.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_touch.h"

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

int legacyKeyCodeFor(const std::string& code, const std::string& key) {
    if (key == "Enter") return 13;
    if (key == "Backspace") return 8;
    if (key == "Tab") return 9;
    if (key == "Escape") return 27;
    if (key == " " || key == "Space") return 32;
    if (key == "ArrowLeft") return 37;
    if (key == "ArrowUp") return 38;
    if (key == "ArrowRight") return 39;
    if (key == "ArrowDown") return 40;
    if (key == "Delete") return 46;
    if (key == "Insert") return 45;
    if (key == "Home") return 36;
    if (key == "End") return 35;
    if (key == "PageUp") return 33;
    if (key == "PageDown") return 34;
    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 65;
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    if (code.rfind("Key", 0) == 0 && code.size() == 4) {
        char c = code[3];
        if (c >= 'A' && c <= 'Z') return c;
    }
    if (code.rfind("Digit", 0) == 0 && code.size() == 6) {
        char c = code[5];
        if (c >= '0' && c <= '9') return c;
    }
    return 0;
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
    Value baseObj = ev::undefined();
    if (dynamic_cast<dom::TouchEvent*>(&e)) {
        baseObj = g_touchEventClass.make(nullptr, [](void*) {});
    } else if (dynamic_cast<dom::GestureEvent*>(&e)) {
        baseObj = g_gestureEventClass.make(nullptr, [](void*) {});
    }
    ObjectBuilder b(ev::isUndefined(baseObj) ? ev::createObject() : baseObj);

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
        b.set("which", ev::fromDouble(m->button() >= 0 ? m->button() + 1 : 0));
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
            b.set("width", ev::fromDouble(1.0));
            b.set("height", ev::fromDouble(1.0));
            double pressure = m->pressure();
            if (pressure < 0.0) pressure = m->buttons() != 0 ? 0.5 : 0.0;
            b.set("pressure", ev::fromDouble(pressure));
            b.set("tangentialPressure", ev::fromDouble(0.0));
            b.set("tiltX", ev::fromDouble(0.0));
            b.set("tiltY", ev::fromDouble(0.0));
            b.set("twist", ev::fromDouble(0.0));
        }
        if (auto* w = dynamic_cast<dom::WheelEvent*>(&e)) {
            b.set("deltaX", ev::fromDouble(w->deltaX()));
            b.set("deltaY", ev::fromDouble(w->deltaY()));
            b.set("deltaZ", ev::fromDouble(w->deltaZ()));
            b.set("deltaMode", ev::fromDouble(w->deltaMode()));
        }
    }

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

        b.set("dataTransfer", dt.get());

        if (drag->type() == "dragend") {
            g_dragSession.data.clear();
            g_dragSession.effectAllowed = "all";
            g_dragSession.dropEffect = "none";
        }
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
        int kc = legacyKeyCodeFor(k->code(), k->key());
        b.set("keyCode", ev::fromDouble(kc));
        b.set("which", ev::fromDouble(kc));
        b.set("charCode", ev::fromDouble(0.0));
    }

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

    if (auto* inp = dynamic_cast<dom::InputEvent*>(&e)) {
        if (inp->data().empty()) {
            b.set("data", ev::null());
        } else {
            b.set("data", ev::fromUtf8(inp->data()));
        }
        b.set("inputType", ev::fromUtf8(inp->inputType()));
        b.set("isComposing", ev::fromBool(inp->isComposing()));
    }

    if (auto* comp = dynamic_cast<dom::CompositionEvent*>(&e)) {
        b.set("data", ev::fromUtf8(comp->data()));
    }

    if (auto* clip = dynamic_cast<dom::ClipboardEvent*>(&e)) {
        auto textHolder = std::make_shared<std::string>(clip->clipboardText());
        ObjectBuilder dt;
        dt.def("getData", 1, [textHolder](Value, std::span<const Value> a) {
            Value fV = argAt(a, 0);
            if (ev::isObject(fV) || ev::isUndefined(fV)) return ev::fromUtf8("");
            std::string fmt = ev::toUtf8(fV);
            for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fmt == "text" || fmt == "text/plain") return ev::fromUtf8(*textHolder);
            return ev::fromUtf8("");
        });
        dt.def("setData", 2, [textHolder](Value, std::span<const Value> a) {
            if (a.size() >= 2) {
                Value fV = argAt(a, 0);
                std::string fmt = (!ev::isObject(fV) && !ev::isUndefined(fV)) ? ev::toUtf8(fV) : "";
                for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (fmt == "text" || fmt == "text/plain") {
                    Value dataV = argAt(a, 1);
                    *textHolder = (!ev::isObject(dataV) && !ev::isUndefined(dataV)) ? ev::toUtf8(dataV) : "";
                }
            }
            return ev::undefined();
        });
        dt.def("clearData", 1, [textHolder](Value, std::span<const Value>) {
            textHolder->clear();
            return ev::undefined();
        });
        std::vector<std::string> typeList;
        if (!textHolder->empty()) typeList.push_back("text/plain");
        Value typesArr = hostArrayOf(typeList.size(), [&typeList](size_t i) {
            return ev::fromUtf8(typeList[i]);
        });
        dt.set("types", typesArr);
        b.set("clipboardData", dt.get());
    }

    // The three write-throughs. `live` is captured by value: the closures
    // outlive this function (they live on the event object), and the box is
    // what tells them whether the event still exists.
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
    bool capture = false;
    bool once = false;
};

std::vector<ElementListener>& registrations() {
    static auto* list = new std::vector<ElementListener>();
    return *list;
}

} // namespace

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
    ev::Persistent evtObj(buildEventValue(evt, live));
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
    EventSpec spec;
    if (!readEventSpec(desc, what, spec)) return ev::undefined();
    engine::Engine* engine = hostEngine();
    dom::Element* el = source();
    if (!engine || !el) {
        return ev::throwError(std::string(what) +
                              ".dispatchEvent: no element to dispatch at");
    }
    const bool notPrevented = dispatchEventSpec(spec, [engine, el](dom::Event& evt) {
        engine->dispatchElementEvent(el, evt);
    });
    if (spec.type == "click" && notPrevented) {
        runAnchorDownload(el);
    }
    return ev::fromBool(notPrevented);
}

Value hostDispatchToWindow(Value desc) {
    EventSpec spec;
    if (!readEventSpec(desc, "window", spec)) return ev::undefined();
    engine::Engine* engine = hostEngine();
    if (!engine) return ev::throwError("window.dispatchEvent: no engine");
    return ev::fromBool(dispatchEventSpec(spec, [engine](dom::Event& evt) {
        engine->dispatchWindowEvent(evt);
    }));
}

}  // namespace bro::bronze_host
