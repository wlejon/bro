#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "engine/engine.h"
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace bro::bronze_host {

namespace {

struct GizmoCallbacks {
    ev::Persistent cbPosition;
    ev::Persistent cbOrientation;
    ev::Persistent cbBeginDrag;
    ev::Persistent cbTranslate;
    ev::Persistent cbRotate;
    ev::Persistent cbScale;
    ev::Persistent cbEndDrag;
    ev::Persistent cbHoverChange;

    void clear() {
        cbPosition.set(ev::undefined());
        cbOrientation.set(ev::undefined());
        cbBeginDrag.set(ev::undefined());
        cbTranslate.set(ev::undefined());
        cbRotate.set(ev::undefined());
        cbScale.set(ev::undefined());
        cbEndDrag.set(ev::undefined());
        cbHoverChange.set(ev::undefined());
    }
};

static GizmoCallbacks s_cbs;

static void parseHexColor(std::string_view s, float (&out)[4]) {
    if (s.empty() || s[0] != '#') return;
    unsigned long val = std::strtoul(s.data() + 1, nullptr, 16);
    if (s.size() == 7) {
        out[0] = ((val >> 16) & 0xFF) / 255.0f;
        out[1] = ((val >>  8) & 0xFF) / 255.0f;
        out[2] = (val & 0xFF) / 255.0f;
        out[3] = 1.0f;
    } else if (s.size() == 9) {
        out[0] = ((val >> 24) & 0xFF) / 255.0f;
        out[1] = ((val >> 16) & 0xFF) / 255.0f;
        out[2] = ((val >>  8) & 0xFF) / 255.0f;
        out[3] = (val & 0xFF) / 255.0f;
    }
}

} // namespace

Value makeBroGizmoValue() {
    ObjectBuilder b;

    b.accessor("visible", [](Value, std::span<const Value>) -> Value {
        auto* e = hostEngine();
        return ev::fromBool(e ? e->gizmo().visible() : false);
    }, nullptr);

    b.accessor("dragging", [](Value, std::span<const Value>) -> Value {
        auto* e = hostEngine();
        return ev::fromBool(e ? e->gizmo().isDragging() : false);
    }, nullptr);

    b.accessor("hovered", [](Value, std::span<const Value>) -> Value {
        auto* e = hostEngine();
        if (!e) return ev::null();
        switch (e->gizmo().hovered()) {
            case engine::GizmoAxis::X:      return ev::fromUtf8("x");
            case engine::GizmoAxis::Y:      return ev::fromUtf8("y");
            case engine::GizmoAxis::Z:      return ev::fromUtf8("z");
            case engine::GizmoAxis::XY:     return ev::fromUtf8("xy");
            case engine::GizmoAxis::YZ:     return ev::fromUtf8("yz");
            case engine::GizmoAxis::XZ:     return ev::fromUtf8("xz");
            case engine::GizmoAxis::Center: return ev::fromUtf8("center");
            case engine::GizmoAxis::View:   return ev::fromUtf8("view");
            case engine::GizmoAxis::None:
            default:                        return ev::null();
        }
    }, nullptr);

    b.def("show", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        if (e) e->gizmo().show();
        return ev::undefined();
    });

    b.def("hide", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        if (e) e->gizmo().hide();
        return ev::undefined();
    });

    b.def("setMode", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (e && !a.empty() && ev::isString(a[0])) {
            std::string s = ev::toUtf8(a[0]);
            if (s == "translate")      e->gizmo().setMode(engine::GizmoMode::Translate);
            else if (s == "rotate")    e->gizmo().setMode(engine::GizmoMode::Rotate);
            else if (s == "scale")     e->gizmo().setMode(engine::GizmoMode::Scale);
        }
        return ev::undefined();
    });

    b.def("setSpace", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (e && !a.empty() && ev::isString(a[0])) {
            std::string s = ev::toUtf8(a[0]);
            if (s == "local") e->gizmo().setSpace(engine::GizmoSpace::Local);
            else              e->gizmo().setSpace(engine::GizmoSpace::World);
        }
        return ev::undefined();
    });

    b.def("setPosition", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (e && a.size() >= 3) {
            float x = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float z = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            e->gizmo().setPosition(x, y, z);
        }
        return ev::undefined();
    });

    b.def("setOrientation", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (e && a.size() >= 4) {
            float x = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float z = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float w = ev::isNumber(a[3]) ? static_cast<float>(ev::toDouble(a[3])) : 1.0f;
            e->gizmo().setOrientation(bromath::Quat(x, y, z, w));
        }
        return ev::undefined();
    });

    b.def("configure", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine();
        if (!e) return ev::undefined();
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("gizmo.configure() requires an options object");

        auto& cfg = e->gizmo().config();
        Value szVal = ev::getProperty(a[0], "size");
        if (ev::isNumber(szVal)) cfg.targetPixelSize = static_cast<float>(ev::toDouble(szVal));
        Value emVal = ev::getProperty(a[0], "emissive");
        if (ev::isNumber(emVal)) cfg.emissive = static_cast<float>(ev::toDouble(emVal));
        Value emhVal = ev::getProperty(a[0], "emissiveHover");
        if (ev::isNumber(emhVal)) cfg.emissiveHover = static_cast<float>(ev::toDouble(emhVal));
        Value aotVal = ev::getProperty(a[0], "alwaysOnTop");
        if (ev::isBool(aotVal)) cfg.alwaysOnTop = ev::toBool(aotVal);

        Value colors = ev::getProperty(a[0], "colors");
        if (ev::isObject(colors)) {
            Value xv = ev::getProperty(colors, "x");
            if (ev::isString(xv)) parseHexColor(ev::toUtf8(xv), cfg.colorX);
            Value yv = ev::getProperty(colors, "y");
            if (ev::isString(yv)) parseHexColor(ev::toUtf8(yv), cfg.colorY);
            Value zv = ev::getProperty(colors, "z");
            if (ev::isString(zv)) parseHexColor(ev::toUtf8(zv), cfg.colorZ);
            Value hv = ev::getProperty(colors, "hover");
            if (ev::isString(hv)) parseHexColor(ev::toUtf8(hv), cfg.colorHover);
            Value av = ev::getProperty(colors, "active");
            if (ev::isString(av)) parseHexColor(ev::toUtf8(av), cfg.colorActive);
        }
        return ev::undefined();
    });

    b.def("attach", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine();
        if (!e) return ev::undefined();
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("gizmo.attach() requires an object");

        e->gizmo().clearCallbacks();
        s_cbs.clear();

        Value opts = a[0];

        Value pos = ev::getProperty(opts, "position");
        if (ev::isFunction(pos)) {
            s_cbs.cbPosition.set(pos);
            e->gizmo().onGetPosition = []() -> bromath::Vec3 {
                Value fn = s_cbs.cbPosition.get();
                if (ev::isFunction(fn)) {
                    auto r = ev::call(fn, ev::undefined(), {});
                    if (!r.thrown && ev::isObject(r.value)) {
                        bromath::Vec3 p{0, 0, 0};
                        if (readVec3FromValue(r.value, p)) return p;
                    }
                }
                return {0, 0, 0};
            };
        }

        Value orient = ev::getProperty(opts, "orientation");
        if (ev::isFunction(orient)) {
            s_cbs.cbOrientation.set(orient);
            e->gizmo().onGetOrientation = []() -> bromath::Quat {
                Value fn = s_cbs.cbOrientation.get();
                if (ev::isFunction(fn)) {
                    auto r = ev::call(fn, ev::undefined(), {});
                    if (!r.thrown && ev::isObject(r.value)) {
                        bromath::Quat q{0, 0, 0, 1};
                        if (readQuatFromValue(r.value, q)) return q;
                    }
                }
                return {0, 0, 0, 1};
            };
        }

        Value bDrag = ev::getProperty(opts, "beginDrag");
        if (ev::isFunction(bDrag)) {
            s_cbs.cbBeginDrag.set(bDrag);
            e->gizmo().onBeginDrag = []() {
                Value fn = s_cbs.cbBeginDrag.get();
                if (ev::isFunction(fn)) {
                    ev::call(fn, ev::undefined(), {});
                }
            };
        }

        Value trans = ev::getProperty(opts, "translate");
        if (ev::isFunction(trans)) {
            s_cbs.cbTranslate.set(trans);
            e->gizmo().onTranslate = [](const bromath::Vec3& d) {
                Value fn = s_cbs.cbTranslate.get();
                if (ev::isFunction(fn)) {
                    Value args[3] = { ev::fromDouble(d.x), ev::fromDouble(d.y), ev::fromDouble(d.z) };
                    ev::call(fn, ev::undefined(), args);
                }
            };
        }

        Value rot = ev::getProperty(opts, "rotate");
        if (ev::isFunction(rot)) {
            s_cbs.cbRotate.set(rot);
            e->gizmo().onRotate = [](const bromath::Quat& q) {
                Value fn = s_cbs.cbRotate.get();
                if (ev::isFunction(fn)) {
                    Value args[4] = { ev::fromDouble(q.x), ev::fromDouble(q.y), ev::fromDouble(q.z), ev::fromDouble(q.w) };
                    ev::call(fn, ev::undefined(), args);
                }
            };
        }

        Value sc = ev::getProperty(opts, "scale");
        if (ev::isFunction(sc)) {
            s_cbs.cbScale.set(sc);
            e->gizmo().onScale = [](const bromath::Vec3& s) {
                Value fn = s_cbs.cbScale.get();
                if (ev::isFunction(fn)) {
                    Value args[3] = { ev::fromDouble(s.x), ev::fromDouble(s.y), ev::fromDouble(s.z) };
                    ev::call(fn, ev::undefined(), args);
                }
            };
        }

        Value eDrag = ev::getProperty(opts, "endDrag");
        if (ev::isFunction(eDrag)) {
            s_cbs.cbEndDrag.set(eDrag);
            e->gizmo().onEndDrag = [](bool committed) {
                Value fn = s_cbs.cbEndDrag.get();
                if (ev::isFunction(fn)) {
                    Value arg = ev::fromBool(committed);
                    ev::call(fn, ev::undefined(), std::span<const Value>(&arg, 1));
                }
            };
        }

        Value hChg = ev::getProperty(opts, "hoverChange");
        if (ev::isFunction(hChg)) {
            s_cbs.cbHoverChange.set(hChg);
            e->gizmo().onHoverChange = []() {
                Value fn = s_cbs.cbHoverChange.get();
                if (ev::isFunction(fn)) {
                    ev::call(fn, ev::undefined(), {});
                }
            };
        }

        e->gizmo().show();
        return ev::undefined();
    });

    b.def("detach", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        if (e) {
            e->gizmo().clearCallbacks();
            e->gizmo().hide();
        }
        s_cbs.clear();
        return ev::undefined();
    });

    return b.get();
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
