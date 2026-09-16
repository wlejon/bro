// native_scene_gizmo.cpp — Gizmo native implementations.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/gizmo/native_gizmo_decl.h"
#include "engine/engine.h"
#include "engine/gizmo.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace bro::bronze_host {

bool registerNatives_gizmo(std::string* error);

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

// Named functions rather than lambdas at the assignment sites: MSVC refuses a
// captureless lambda that returns a C++ class type when it is defined inside
// an extern "C" function (C2526 on the generated invoker).
static bromath::Vec3 callPositionHandler() {
    Value fn = s_cbs.cbPosition.get();
    if (ev::isFunction(fn)) {
        auto r = ev::call(fn, ev::undefined(), {});
        if (!r.thrown && ev::isObject(r.value)) {
            Value e0 = ev::getElement(r.value, 0);
            Value e1 = ev::getElement(r.value, 1);
            Value e2 = ev::getElement(r.value, 2);
            if (ev::isNumber(e0) && ev::isNumber(e1) && ev::isNumber(e2)) {
                return {static_cast<float>(ev::toDouble(e0)),
                        static_cast<float>(ev::toDouble(e1)),
                        static_cast<float>(ev::toDouble(e2))};
            }
        }
    }
    return {0, 0, 0};
}

static bromath::Quat callOrientationHandler() {
    Value fn = s_cbs.cbOrientation.get();
    if (ev::isFunction(fn)) {
        auto r = ev::call(fn, ev::undefined(), {});
        if (!r.thrown && ev::isObject(r.value)) {
            Value e0 = ev::getElement(r.value, 0);
            Value e1 = ev::getElement(r.value, 1);
            Value e2 = ev::getElement(r.value, 2);
            Value e3 = ev::getElement(r.value, 3);
            if (ev::isNumber(e0) && ev::isNumber(e1) && ev::isNumber(e2) && ev::isNumber(e3)) {
                return {static_cast<float>(ev::toDouble(e0)),
                        static_cast<float>(ev::toDouble(e1)),
                        static_cast<float>(ev::toDouble(e2)),
                        static_cast<float>(ev::toDouble(e3))};
            }
        }
    }
    return {0, 0, 0, 1};
}

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

}  // namespace

extern "C" const char* bro_gizmo_hovered_get(void);

bool registerGizmoNatives(std::string* error) {
    if (!registerNatives_gizmo(error)) return false;
    using namespace natives;
    return getter("__bro_native.gizmo.hovered", (void*)&bro_gizmo_hovered_get, "str", error);
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

const char* bro_gizmo_hovered_get(void) {
    auto* e = hostEngine();
    if (!e) return "";
    switch (e->gizmo().hovered()) {
        case engine::GizmoAxis::X: return "x";
        case engine::GizmoAxis::Y: return "y";
        case engine::GizmoAxis::Z: return "z";
        case engine::GizmoAxis::XY: return "xy";
        case engine::GizmoAxis::YZ: return "yz";
        case engine::GizmoAxis::XZ: return "xz";
        case engine::GizmoAxis::View: return "view";
        case engine::GizmoAxis::Center: return "center";
        default: return "";
    }
}

bool bro_gizmo_visible_get(void) {
    auto* e = hostEngine();
    return e ? e->gizmo().visible() : false;
}

bool bro_gizmo_dragging_get(void) {
    auto* e = hostEngine();
    return e ? e->gizmo().isDragging() : false;
}

void bro_gizmo_show(void) {
    auto* e = hostEngine();
    if (e) e->gizmo().show();
}

void bro_gizmo_hide(void) {
    auto* e = hostEngine();
    if (e) e->gizmo().hide();
}

void bro_gizmo_setMode(const char* mode) {
    auto* e = hostEngine();
    if (!e || !mode) return;
    std::string s = mode;
    if (s == "translate")   e->gizmo().setMode(engine::GizmoMode::Translate);
    else if (s == "rotate") e->gizmo().setMode(engine::GizmoMode::Rotate);
    else if (s == "scale")  e->gizmo().setMode(engine::GizmoMode::Scale);
}

void bro_gizmo_setSpace(const char* space) {
    auto* e = hostEngine();
    if (!e || !space) return;
    std::string s = space;
    if (s == "local") e->gizmo().setSpace(engine::GizmoSpace::Local);
    else              e->gizmo().setSpace(engine::GizmoSpace::World);
}

void bro_gizmo_setPosition(double x, double y, double z) {
    auto* e = hostEngine();
    if (e) e->gizmo().setPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void bro_gizmo_setOrientation(double x, double y, double z, double w) {
    auto* e = hostEngine();
    if (e) e->gizmo().setOrientation(bromath::Quat(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)));
}

void bro_gizmo_configure(bool config_size_given, double config_size,
                         bool config_colors_x_given, const char* config_colors_x,
                         bool config_colors_y_given, const char* config_colors_y,
                         bool config_colors_z_given, const char* config_colors_z,
                         bool config_colors_hover_given, const char* config_colors_hover,
                         bool config_colors_active_given, const char* config_colors_active,
                         bool config_emissive_given, double config_emissive,
                         bool config_emissiveHover_given, double config_emissiveHover,
                         bool config_alwaysOnTop_given, bool config_alwaysOnTop) {
    auto* e = hostEngine();
    if (!e) return;
    auto& cfg = e->gizmo().config();
    if (config_size_given) cfg.targetPixelSize = static_cast<float>(config_size);
    if (config_emissive_given) cfg.emissive = static_cast<float>(config_emissive);
    if (config_emissiveHover_given) cfg.emissiveHover = static_cast<float>(config_emissiveHover);
    if (config_alwaysOnTop_given) cfg.alwaysOnTop = config_alwaysOnTop;

    if (config_colors_x_given && config_colors_x) parseHexColor(config_colors_x, cfg.colorX);
    if (config_colors_y_given && config_colors_y) parseHexColor(config_colors_y, cfg.colorY);
    if (config_colors_z_given && config_colors_z) parseHexColor(config_colors_z, cfg.colorZ);
    if (config_colors_hover_given && config_colors_hover) parseHexColor(config_colors_hover, cfg.colorHover);
    if (config_colors_active_given && config_colors_active) parseHexColor(config_colors_active, cfg.colorActive);
}

void bro_gizmo_attach(uint64_t handlers_position, uint64_t handlers_orientation,
                      uint64_t handlers_beginDrag, uint64_t handlers_translate,
                      uint64_t handlers_rotate, uint64_t handlers_scale,
                      uint64_t handlers_endDrag, uint64_t handlers_hoverChange) {
    auto* e = hostEngine();
    if (!e) return;

    e->gizmo().clearCallbacks();
    s_cbs.clear();

    Value pos = ev::fromBits(handlers_position);
    if (ev::isFunction(pos)) {
        s_cbs.cbPosition.set(pos);
        e->gizmo().onGetPosition = &callPositionHandler;
    }

    Value orient = ev::fromBits(handlers_orientation);
    if (ev::isFunction(orient)) {
        s_cbs.cbOrientation.set(orient);
        e->gizmo().onGetOrientation = &callOrientationHandler;
    }

    Value bDrag = ev::fromBits(handlers_beginDrag);
    if (ev::isFunction(bDrag)) {
        s_cbs.cbBeginDrag.set(bDrag);
        e->gizmo().onBeginDrag = []() {
            Value fn = s_cbs.cbBeginDrag.get();
            if (ev::isFunction(fn)) ev::call(fn, ev::undefined(), {});
        };
    }

    Value trans = ev::fromBits(handlers_translate);
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

    Value rot = ev::fromBits(handlers_rotate);
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

    Value sc = ev::fromBits(handlers_scale);
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

    Value eDrag = ev::fromBits(handlers_endDrag);
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

    Value hChg = ev::fromBits(handlers_hoverChange);
    if (ev::isFunction(hChg)) {
        s_cbs.cbHoverChange.set(hChg);
        e->gizmo().onHoverChange = []() {
            Value fn = s_cbs.cbHoverChange.get();
            if (ev::isFunction(fn)) ev::call(fn, ev::undefined(), {});
        };
    }

    e->gizmo().show();
}

void bro_gizmo_detach(void) {
    auto* e = hostEngine();
    if (e) {
        e->gizmo().clearCallbacks();
        e->gizmo().hide();
    }
    s_cbs.clear();
}

}  // extern "C"
