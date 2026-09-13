#include "bronze_host/host_headless_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#if BRO_WITH_3D
#include "bronze_host/host_scene_internal.h"
#endif
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"

#include <vector>
#include <string>

namespace bro::bronze_host {

namespace {

std::vector<std::string> g_callLog;

struct SeenEvent {
    bool any = false;
    std::string type;
    std::string targetTag;
    std::string currentTargetTag;
    int eventPhase = 0;
    bool bubbles = false;
    bool cancelable = false;
    bool isTrusted = false;
    bool defaultPrevented = false;
    double clientX = 0, clientY = 0;
    std::string key;
} g_lastEvent;

struct ListenerBehaviour {
    bool preventDefault = false;
    bool stopPropagation = false;
    bool stopImmediatePropagation = false;
};

std::string elementLabel(dom::Element* el) {
    if (!el) return "";
    std::string id = el->getAttribute("id");
    return id.empty() ? el->tagName() : ("#" + id);
}

dom::ListenerOptions readOptions(Value obj, ListenerBehaviour* behaviour) {
    dom::ListenerOptions opts;
    if (!ev::isObject(obj)) return opts;
    auto flag = [&](const char* name) {
        Value v = ev::getProperty(obj, name);
        return !ev::isUndefined(v) && ev::toBool(v);
    };
    opts.capture = flag("capture");
    opts.once = flag("once");
    if (behaviour) {
        behaviour->preventDefault = flag("preventDefault");
        behaviour->stopPropagation = flag("stopPropagation");
        behaviour->stopImmediatePropagation = flag("stopImmediatePropagation");
    }
    return opts;
}

dom::EventCallback makeLoggingListener(std::string tag, ListenerBehaviour behaviour) {
    return [tag = std::move(tag), behaviour](dom::Event& ev) {
        g_callLog.push_back(tag);

        g_lastEvent = SeenEvent{};
        g_lastEvent.any = true;
        g_lastEvent.type = ev.type();
        g_lastEvent.targetTag = elementLabel(ev.target());
        g_lastEvent.currentTargetTag = elementLabel(ev.currentTarget());
        g_lastEvent.eventPhase = ev.eventPhase();
        g_lastEvent.bubbles = ev.bubbles();
        g_lastEvent.cancelable = ev.cancelable();
        g_lastEvent.isTrusted = ev.isTrusted();
        g_lastEvent.defaultPrevented = ev.defaultPrevented();
        if (auto* me = dynamic_cast<dom::MouseEvent*>(&ev)) {
            g_lastEvent.clientX = me->clientX();
            g_lastEvent.clientY = me->clientY();
        }
        if (auto* ke = dynamic_cast<dom::KeyboardEvent*>(&ev)) {
            g_lastEvent.key = ke->key();
        }

        if (behaviour.preventDefault) ev.preventDefault();
        if (behaviour.stopPropagation) ev.stopPropagation();
        if (behaviour.stopImmediatePropagation) ev.stopImmediatePropagation();
    };
}

} // namespace

void installHeadlessTestHooks(engine::Engine& engine) {
    ObjectBuilder host;

    host.def("engineResolvedAtInstall", 0, [](Value, std::span<const Value>) {
        return ev::fromBool(true);
    });

    host.def("createCanvas", 3, [](Value, std::span<const Value> a) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        auto* doc = eng->document();
        if (!doc) return ev::throwError("__host: no document");
        auto* parent = doc->body() ? doc->body() : doc->documentElement();
        if (!parent) return ev::throwError("__host: document has no body");

        std::string id = a.size() >= 1 && ev::isString(a[0]) ? ev::toUtf8(a[0]) : "";
        int32_t w = a.size() >= 2 && ev::isNumber(a[1]) ? static_cast<int32_t>(ev::toDouble(a[1])) : 128;
        int32_t h = a.size() >= 3 && ev::isNumber(a[2]) ? static_cast<int32_t>(ev::toDouble(a[2])) : 128;

        auto* canvas = doc->createElement("canvas");
        if (!canvas) return ev::throwError("__host: createElement failed");
        if (!id.empty()) canvas->setAttribute("id", id);
        canvas->setAttribute("width", std::to_string(w));
        canvas->setAttribute("height", std::to_string(h));
        canvas->style().setProperty("width", std::to_string(w) + "px");
        canvas->style().setProperty("height", std::to_string(h) + "px");
        canvas->style().setProperty("display", "block");

        parent->appendChild(canvas);
        return hostElementValue(canvas);
    });

    host.def("sceneContext", 1, [](Value, std::span<const Value> a) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        if (a.empty()) return ev::throwTypeError("__host.sceneContext(canvas)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.sceneContext: not an Element");

        auto* graph = eng->createSceneContext(el);
        if (!graph) return ev::null();
#if BRO_WITH_3D
        return createSceneGraphValue(graph, el);
#else
        return ev::null();
#endif
    });

    host.def("sceneContextCount", 0, [](Value, std::span<const Value>) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        return ev::fromDouble(static_cast<double>(eng->sceneContextCount()));
    });

    host.def("sceneLink", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("__host.sceneLink(canvas)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.sceneLink: not an Element");

        ObjectBuilder out;
        out.set("graph", ev::fromBool(el->sceneGraph() != nullptr));
        out.set("fboTexture", ev::fromDouble(static_cast<double>(el->sceneGraphFBOTexture())));
        return out.get();
    });

    host.def("hasJsListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::throwTypeError("__host.hasJsListener(element, type)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.hasJsListener: not an Element");
        std::string type = ev::toUtf8(a[1]);
        return ev::fromBool(el->hasJsListener(type));
    });

    host.def("addWindowListener", 3, [](Value, std::span<const Value> a) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        if (a.size() < 2 || !ev::isString(a[0]) || !ev::isString(a[1])) {
            return ev::throwTypeError("__host.addWindowListener: type and tag must be strings");
        }
        std::string type = ev::toUtf8(a[0]);
        std::string tag = ev::toUtf8(a[1]);
        ListenerBehaviour behaviour;
        auto opts = readOptions(a.size() >= 3 ? a[2] : ev::undefined(), &behaviour);

        auto handle = eng->addWindowEventListener(type, makeLoggingListener(std::move(tag), behaviour), opts);
        if (!handle) return ev::throwError("__host: addWindowEventListener failed");
        return ev::fromDouble(static_cast<double>(handle.id));
    });

    host.def("removeWindowListener", 1, [](Value, std::span<const Value> a) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        uint64_t id = a.empty() ? 0 : static_cast<uint64_t>(ev::toDouble(a[0]));
        return ev::fromBool(eng->removeWindowEventListener(dom::ListenerHandle{id}));
    });

    host.def("addElementListener", 4, [](Value, std::span<const Value> a) {
        if (a.size() < 3) return ev::throwTypeError("__host.addElementListener(el, type, tag, opts)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.addElementListener: not an Element");
        if (!ev::isString(a[1]) || !ev::isString(a[2])) {
            return ev::throwTypeError("__host.addElementListener: type and tag must be strings");
        }
        std::string type = ev::toUtf8(a[1]);
        std::string tag = ev::toUtf8(a[2]);
        ListenerBehaviour behaviour;
        auto opts = readOptions(a.size() >= 4 ? a[3] : ev::undefined(), &behaviour);

        auto handle = el->addEventListener(type, makeLoggingListener(std::move(tag), behaviour), opts);
        if (!handle) return ev::throwError("__host: addEventListener failed");
        return ev::fromDouble(static_cast<double>(handle.id));
    });

    host.def("removeElementListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::throwTypeError("__host.removeElementListener(el, id)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.removeElementListener: not an Element");
        uint64_t id = static_cast<uint64_t>(ev::toDouble(a[1]));
        return ev::fromBool(el->removeEventListener(dom::ListenerHandle{id}));
    });

    host.def("note", 1, [](Value, std::span<const Value> a) {
        if (!a.empty() && ev::isString(a[0])) {
            g_callLog.push_back(ev::toUtf8(a[0]));
        }
        return ev::undefined();
    });

    host.def("log", 0, [](Value, std::span<const Value>) {
        Value arr = ev::parseJson("[]").value;
        for (size_t i = 0; i < g_callLog.size(); ++i) {
            ev::setElement(arr, static_cast<uint32_t>(i), ev::fromUtf8(g_callLog[i]));
        }
        return arr;
    });

    host.def("clearLog", 0, [](Value, std::span<const Value>) {
        g_callLog.clear();
        g_lastEvent = SeenEvent{};
        return ev::undefined();
    });

    host.def("lastEvent", 0, [](Value, std::span<const Value>) {
        if (!g_lastEvent.any) return ev::null();
        ObjectBuilder o;
        o.set("type", ev::fromUtf8(g_lastEvent.type));
        o.set("target", ev::fromUtf8(g_lastEvent.targetTag));
        o.set("currentTarget", ev::fromUtf8(g_lastEvent.currentTargetTag));
        o.set("eventPhase", ev::fromDouble(g_lastEvent.eventPhase));
        o.set("bubbles", ev::fromBool(g_lastEvent.bubbles));
        o.set("cancelable", ev::fromBool(g_lastEvent.cancelable));
        o.set("isTrusted", ev::fromBool(g_lastEvent.isTrusted));
        o.set("defaultPrevented", ev::fromBool(g_lastEvent.defaultPrevented));
        o.set("clientX", ev::fromDouble(g_lastEvent.clientX));
        o.set("clientY", ev::fromDouble(g_lastEvent.clientY));
        o.set("key", ev::fromUtf8(g_lastEvent.key));
        return o.get();
    });

    ev::registerGlobal("__host", host.get());
}

} // namespace bro::bronze_host
