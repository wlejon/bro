#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "platform/sdl_window.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

static engine::Settings* getSettings() {
    auto* eng = hostEngine();
    return eng ? eng->settings() : nullptr;
}

static Value settingsValueToJS(const std::string& key, const std::string& value) {
    if (value.empty()) return ev::undefined();
    if (key.find("fullscreen") != std::string::npos ||
        key.find("vsync") != std::string::npos ||
        key.find("resizable") != std::string::npos ||
        key.find("muted") != std::string::npos) {
        return ev::fromBool(value == "true");
    }
    if (key.find("width") != std::string::npos ||
        key.find("height") != std::string::npos ||
        key.find("overlayToggleKey") != std::string::npos) {
        try { return ev::fromDouble(std::stoi(value)); }
        catch (...) { return ev::fromUtf8(value); }
    }
    if (key.find("Volume") != std::string::npos ||
        key.find("Speed") != std::string::npos ||
        key.find("Threshold") != std::string::npos ||
        key.find("Distance") != std::string::npos ||
        key.find("Interval") != std::string::npos ||
        key.find("maxFps") != std::string::npos) {
        try { return ev::fromDouble(std::stod(value)); }
        catch (...) { return ev::fromUtf8(value); }
    }
    return ev::fromUtf8(value);
}

static Value buildActionArray(const std::vector<engine::ActionBinding>& actions) {
    return hostArrayOf(actions.size(), [&actions](size_t i) -> Value {
        ObjectBuilder obj;
        obj.set("action", ev::fromUtf8(actions[i].action));
        const auto& keys = actions[i].keys;
        Value keysArr = hostArrayOf(keys.size(), [&keys](size_t j) -> Value {
            return ev::fromUtf8(keys[j]);
        });
        obj.set("keys", keysArr);
        return obj.get();
    });
}

static std::vector<std::string> parseKeysArray(Value arr) {
    std::vector<std::string> keys;
    if (!ev::isObject(arr)) return keys;
    Value lenVal = ev::getProperty(arr, "length");
    if (!ev::isNumber(lenVal)) return keys;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
    keys.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value elem = ev::getElement(arr, i);
        if (ev::isString(elem)) keys.push_back(ev::toUtf8(elem));
    }
    return keys;
}

} // namespace

Value makeBroSettingsValue() {
    ObjectBuilder s;

    s.def("get", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string key = ev::toUtf8(a[0]);
        std::string val = store->getString(key);
        return settingsValueToJS(key, val);
    });

    s.def("getAll", 1, [](Value, std::span<const Value> a) -> Value {
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string category = (!a.empty() && ev::isString(a[0])) ? ev::toUtf8(a[0]) : "";

        auto addGraphics = [store](ObjectBuilder& obj) {
            auto& g = store->graphics();
            obj.set("width", ev::fromDouble(g.width));
            obj.set("height", ev::fromDouble(g.height));
            obj.set("fullscreen", ev::fromBool(g.fullscreen));
            obj.set("vsync", ev::fromBool(g.vsync));
            obj.set("resizable", ev::fromBool(g.resizable));
            obj.set("maxFrameIntervalMs", ev::fromDouble(g.maxFrameIntervalMs));
            obj.set("maxFps", ev::fromDouble(g.maxFps));
        };
        auto addAudio = [store](ObjectBuilder& obj) {
            auto& au = store->audio();
            obj.set("masterVolume", ev::fromDouble(au.masterVolume));
            obj.set("musicVolume", ev::fromDouble(au.musicVolume));
            obj.set("sfxVolume", ev::fromDouble(au.sfxVolume));
            obj.set("muted", ev::fromBool(au.muted));
        };
        auto addInput = [store](ObjectBuilder& obj) {
            auto& inp = store->input();
            obj.set("scrollSpeed", ev::fromDouble(inp.scrollSpeed));
            obj.set("doubleClickThresholdMs", ev::fromDouble(inp.doubleClickThresholdMs));
            obj.set("doubleClickDistancePx", ev::fromDouble(inp.doubleClickDistancePx));
            obj.set("overlayToggleKey", ev::fromDouble(static_cast<double>(inp.overlayToggleKey)));
        };
        auto addAppearance = [store](ObjectBuilder& obj) {
            auto& ap = store->appearance();
            obj.set("colorScheme", ev::fromUtf8(ap.colorScheme));
        };

        if (category == "appearance") {
            ObjectBuilder obj; addAppearance(obj); return obj.get();
        }
        if (category == "graphics") {
            ObjectBuilder obj; addGraphics(obj); return obj.get();
        }
        if (category == "audio") {
            ObjectBuilder obj; addAudio(obj); return obj.get();
        }
        if (category == "input") {
            ObjectBuilder obj; addInput(obj); return obj.get();
        }

        ObjectBuilder root;
        { ObjectBuilder g; addGraphics(g); root.set("graphics", g.get()); }
        { ObjectBuilder au; addAudio(au); root.set("audio", au.get()); }
        { ObjectBuilder inp; addInput(inp); root.set("input", inp.get()); }
        { ObjectBuilder ap; addAppearance(ap); root.set("appearance", ap.get()); }
        return root.get();
    });

    s.def("getDefaults", 1, [](Value, std::span<const Value> a) -> Value {
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string category = (!a.empty() && ev::isString(a[0])) ? ev::toUtf8(a[0]) : "";

        auto addGraphics = [store](ObjectBuilder& obj) {
            auto& g = store->graphicsDefaults();
            obj.set("width", ev::fromDouble(g.width));
            obj.set("height", ev::fromDouble(g.height));
            obj.set("fullscreen", ev::fromBool(g.fullscreen));
            obj.set("vsync", ev::fromBool(g.vsync));
            obj.set("resizable", ev::fromBool(g.resizable));
            obj.set("maxFrameIntervalMs", ev::fromDouble(g.maxFrameIntervalMs));
            obj.set("maxFps", ev::fromDouble(g.maxFps));
        };

        if (category == "graphics") {
            ObjectBuilder obj; addGraphics(obj); return obj.get();
        }
        ObjectBuilder root;
        { ObjectBuilder g; addGraphics(g); root.set("graphics", g.get()); }
        return root.get();
    });

    s.def("set", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string key = ev::toUtf8(a[0]);
        if (ev::isBool(a[1])) {
            store->setUser(key, ev::toBool(a[1]));
        } else if (ev::isNumber(a[1])) {
            store->setUser(key, ev::toDouble(a[1]));
        } else if (ev::isString(a[1])) {
            store->setUser(key, ev::toUtf8(a[1]));
        }
        return ev::undefined();
    });

    s.def("setDefault", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string key = ev::toUtf8(a[0]);
        if (ev::isBool(a[1])) {
            store->setDefault(key, ev::toBool(a[1]));
        } else if (ev::isNumber(a[1])) {
            store->setDefault(key, ev::toDouble(a[1]));
        } else if (ev::isString(a[1])) {
            store->setDefault(key, ev::toUtf8(a[1]));
        }
        return ev::undefined();
    });

    s.def("reset", 1, [](Value, std::span<const Value> a) -> Value {
        auto* store = getSettings();
        if (!store) return ev::undefined();
        if (!a.empty() && ev::isString(a[0])) {
            store->resetCategory(ev::toUtf8(a[0]));
        } else {
            store->resetAll();
        }
        return ev::undefined();
    });

    s.def("defineAction", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string action = ev::toUtf8(a[0]);
        auto keys = parseKeysArray(a[1]);
        store->defineAction(action, keys);
        if (a.size() >= 3 && ev::isObject(a[2])) {
            Value dz = ev::getProperty(a[2], "deadzone");
            if (ev::isNumber(dz)) {
                store->setActionDeadzone(action, static_cast<float>(ev::toDouble(dz)));
            }
        }
        return ev::undefined();
    });

    s.def("rebindAction", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (!store) return ev::undefined();
        std::string action = ev::toUtf8(a[0]);
        auto keys = parseKeysArray(a[1]);
        store->rebindAction(action, keys);
        return ev::undefined();
    });

    s.def("resetAction", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::undefined();
        auto* store = getSettings();
        if (store) store->resetAction(ev::toUtf8(a[0]));
        return ev::undefined();
    });

    s.def("resetAllActions", 0, [](Value, std::span<const Value>) -> Value {
        auto* store = getSettings();
        if (store) store->resetAllActions();
        return ev::undefined();
    });

    s.def("getActionKeys", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto* store = getSettings();
        if (!store) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto keys = store->getKeysForAction(ev::toUtf8(a[0]));
        return hostArrayOf(keys.size(), [&keys](size_t i) -> Value {
            return ev::fromUtf8(keys[i]);
        });
    });

    s.def("getKeyAction", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::null();
        auto* store = getSettings();
        if (!store) return ev::null();
        std::string act = store->getActionForKey(ev::toUtf8(a[0]));
        if (act.empty()) return ev::null();
        return ev::fromUtf8(act);
    });

    s.def("getActionStrength", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::fromDouble(0.0);
        auto* eng = hostEngine();
        if (!eng) return ev::fromDouble(0.0);
        float st = eng->actionStrength(ev::toUtf8(a[0]));
        return ev::fromDouble(static_cast<double>(st));
    });

    s.def("isActionPressed", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::fromBool(false);
        auto* eng = hostEngine();
        if (!eng) return ev::fromBool(false);
        return ev::fromBool(eng->actionPressed(ev::toUtf8(a[0])));
    });

    s.def("getActions", 0, [](Value, std::span<const Value>) -> Value {
        auto* store = getSettings();
        if (!store) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        return buildActionArray(store->getActions());
    });

    s.def("getAppActions", 0, [](Value, std::span<const Value>) -> Value {
        auto* store = getSettings();
        if (!store) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        return buildActionArray(store->getAppActions());
    });

    s.def("getDisplayModes", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        auto* win = eng ? eng->window() : nullptr;
        if (!win) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto modes = win->getDisplayModes();
        return hostArrayOf(modes.size(), [&modes](size_t i) -> Value {
            ObjectBuilder obj;
            obj.set("width", ev::fromDouble(modes[i].width));
            obj.set("height", ev::fromDouble(modes[i].height));
            obj.set("refreshRate", ev::fromDouble(modes[i].refreshRate));
            return obj.get();
        });
    });

    return s.get();
}

} // namespace bro::bronze_host
