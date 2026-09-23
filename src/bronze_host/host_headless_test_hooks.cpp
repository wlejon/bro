#include "bronze_host/host_headless_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"

#include <vector>
#include <string>

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif

#if BRO_WITH_VISION
#include "bronze_host/host_vision.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#endif

namespace bro::bronze_host {

#if BRO_WITH_3D
Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas);
#endif

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

#if BRO_WITH_VISION
// __host.visionProbe(width, height, opts?) — bro.vision's job machine
// (runVisionOp / tickVisionJobs) and its rasterizers, driven by a synthetic
// compute instead of a model, because every real vision op needs a
// checkpoint on disk and the machine is bro's own code either way.
//
// The probe is ONE model, busy-keyed like a real one, so the one-op-at-a-time
// rule is observable. The compute writes the ramp plane[i] = i / (n - 1) and
// honours:
//   opts.holdMs   spin (checking cancel) for up to this long first
//   opts.fail     throw this message from the worker
//   opts.kind     which rasterizer builds `image`: "gray" (default,
//                 min/max-normalized), "unit", "rgba", "rgb", "mask",
//                 "normals", "segments"
//   opts.invert   the gray rasterizers' invert flag
//   opts.onDone   the async form, exactly as the real ops take it
// Result: { width, height, plane: Float32Array, min, max, onWorker, image }.
struct VisionProbeJob {
    int w = 0;
    int h = 0;
    double holdMs = 0.0;
    std::string fail;
    std::string kind = "gray";
    bool invert = false;
    std::thread::id caller;
    bool onWorker = false;
    std::vector<float> plane;
};

int g_visionProbeKey = 0;

Value visionProbeBitmap(const VisionProbeJob& j, float& lo, float& hi) {
    const std::size_t n = static_cast<std::size_t>(j.w) * j.h;
    lo = 0.0f;
    hi = 1.0f;
    if (j.kind == "unit") return visionBitmapGrayUnit(j.plane, j.w, j.h, j.invert);
    if (j.kind == "rgba" || j.kind == "rgb") {
        const int ch = j.kind == "rgba" ? 4 : 3;
        std::vector<uint8_t> px(n * ch);
        for (std::size_t i = 0; i < n; ++i) {
            const auto g = static_cast<uint8_t>(std::lround(j.plane[i] * 255.0f));
            for (int c = 0; c < ch; ++c) px[i * ch + c] = g;
            if (ch == 4) px[i * 4 + 3] = 128;
        }
        return ch == 4 ? visionBitmapRGBA(px.data(), j.w, j.h)
                       : visionBitmapRGB(px.data(), j.w, j.h);
    }
    if (j.kind == "mask") {
        std::vector<uint8_t> m(n);
        for (std::size_t i = 0; i < n; ++i) m[i] = j.plane[i] >= 0.5f ? 1 : 0;
        return visionBitmapMask(m.data(), j.w, j.h, 30, 144, 255);
    }
    if (j.kind == "normals") {
        std::vector<float> nchw(n * 3, 0.0f);
        for (std::size_t i = 0; i < n; ++i) nchw[2 * n + i] = 1.0f;  // +Z everywhere
        return visionBitmapNormals(nchw, j.w, j.h);
    }
    if (j.kind == "segments") {
        // One diagonal, corner to corner.
        return visionBitmapSegments({0.0f}, {0.0f}, {static_cast<float>(j.w - 1)},
                                    {static_cast<float>(j.h - 1)}, j.w, j.h);
    }
    return visionBitmapGrayNormalized(j.plane, j.w, j.h, j.invert, lo, hi);
}

Value visionProbe(Value, std::span<const Value> a) {
    if (a.size() < 2 || !ev::isNumber(a[0]) || !ev::isNumber(a[1])) {
        return ev::throwTypeError("__host.visionProbe(width, height, opts?)");
    }
    auto job = std::make_shared<VisionProbeJob>();
    job->w = satCast<int>(ev::toDouble(a[0]));
    job->h = satCast<int>(ev::toDouble(a[1]));
    if (job->w <= 0 || job->h <= 0) return ev::throwTypeError("__host.visionProbe: empty size");
    job->caller = std::this_thread::get_id();

    ev::Persistent opts(a.size() > 2 ? a[2] : ev::undefined());
    if (ev::isObject(opts.get())) {
        Value v = ev::getProperty(opts.get(), "holdMs");
        if (ev::isNumber(v)) job->holdMs = ev::toDouble(v);
        v = ev::getProperty(opts.get(), "fail");
        if (ev::isString(v)) job->fail = ev::toUtf8(v);
        v = ev::getProperty(opts.get(), "kind");
        if (ev::isString(v)) job->kind = ev::toUtf8(v);
        v = ev::getProperty(opts.get(), "invert");
        job->invert = !ev::isUndefined(v) && ev::toBool(v);
    }

    if (!visionMarkBusy(&g_visionProbeKey)) {
        return ev::throwError("visionProbe: an operation is already in flight on this model");
    }
    Value onDone = visionOnDone(opts.get());

    auto compute = [job](const std::atomic<bool>& cancel) {
        job->onWorker = std::this_thread::get_id() != job->caller;
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::microseconds(static_cast<int64_t>(job->holdMs * 1000.0));
        while (job->holdMs > 0.0 && std::chrono::steady_clock::now() < until &&
               !cancel.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!job->fail.empty()) throw std::runtime_error(job->fail);
        const std::size_t n = static_cast<std::size_t>(job->w) * job->h;
        job->plane.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            job->plane[i] = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        }
    };
    auto build = [job]() -> Value {
        float lo = 0.0f, hi = 1.0f;
        ev::Persistent bmp(visionProbeBitmap(*job, lo, hi));
        ObjectBuilder res;
        res.set("width", ev::fromDouble(job->w));
        res.set("height", ev::fromDouble(job->h));
        {
            ev::Persistent p(ev::createTypedArray(ev::elements::Float32,
                                                  static_cast<uint32_t>(job->plane.size())));
            ev::fillTypedArray(p.get(),
                               {reinterpret_cast<const uint8_t*>(job->plane.data()),
                                job->plane.size() * sizeof(float)});
            res.set("plane", p.get());
        }
        res.set("min", ev::fromDouble(lo));
        res.set("max", ev::fromDouble(hi));
        res.set("onWorker", ev::fromBool(job->onWorker));
        res.set("image", bmp.get());
        return res.get();
    };
    auto release = []() { visionClearBusy(&g_visionProbeKey); };
    return runVisionOp(onDone, ev::undefined(), std::move(compute), std::move(build),
                       std::move(release));
}
#endif  // BRO_WITH_VISION

} // namespace

void installHeadlessTestHooks(engine::Engine& engine) {
    ObjectBuilder host;

#if BRO_WITH_VISION
    host.def("visionProbe", 3, visionProbe);
#endif

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
        int32_t w = a.size() >= 2 && ev::isNumber(a[1]) ? satCast<int32_t>(ev::toDouble(a[1])) : 128;
        int32_t h = a.size() >= 3 && ev::isNumber(a[2]) ? satCast<int32_t>(ev::toDouble(a[2])) : 128;

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

    host.def("sceneContextCount", 0, [](Value, std::span<const Value>) {
        auto* eng = hostEngine();
        if (!eng) return ev::throwError("__host: no Engine for this realm");
        return ev::fromDouble(static_cast<double>(eng->sceneContextCount()));
    });

    host.def("sceneContext", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("__host.sceneContext(canvas)");
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("__host.sceneContext: not an Element");
        auto* eng = hostEngine();
        if (!eng) return ev::null();
#if BRO_WITH_3D
        scene::SceneGraph* sg = eng->createSceneContext(el);
        if (!sg) return ev::null();
        return createSceneGraphValue(sg, el);
#else
        return ev::null();
#endif
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
        uint64_t id = a.empty() ? 0 : satCast<uint64_t>(ev::toDouble(a[0]));
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
        uint64_t id = satCast<uint64_t>(ev::toDouble(a[1]));
        return ev::fromBool(el->removeEventListener(dom::ListenerHandle{id}));
    });

    host.def("note", 1, [](Value, std::span<const Value> a) {
        if (!a.empty() && ev::isString(a[0])) {
            g_callLog.push_back(ev::toUtf8(a[0]));
        }
        return ev::undefined();
    });

    host.def("log", 0, [](Value, std::span<const Value>) {
        // fromUtf8 allocates, so the string is built in its own statement and
        // the array is read back from its root after it (embed.h, THE TRAP).
        ev::Persistent arr(ev::parseJson("[]").value);
        for (size_t i = 0; i < g_callLog.size(); ++i) {
            ev::Persistent s(ev::fromUtf8(g_callLog[i]));
            ev::setElement(arr.get(), static_cast<uint32_t>(i), s.get());
        }
        return arr.get();
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
