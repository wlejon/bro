#include "js/settings_bindings.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "platform/sdl_window.h"
#include "util/log.h"
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {


static const char* kSettingsKey = "__bro_settings_ptr";
static const char* kWindowKey = "__bro_settings_window_ptr";

struct SettingsState {
    engine::Settings* store = nullptr;
    platform::Window* window = nullptr;
    engine::Engine* engine = nullptr;
};

static SettingsState* getState(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kSettingsKey);
    SettingsState* state = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        state = reinterpret_cast<SettingsState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return state;
}

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

static JSValue settingsValueToJS(JSContext* ctx, const std::string& key,
                                 const std::string& value) {
    if (value.empty()) return JS_UNDEFINED;
    if (key.find("fullscreen") != std::string::npos ||
        key.find("vsync") != std::string::npos ||
        key.find("resizable") != std::string::npos ||
        key.find("muted") != std::string::npos) {
        return JS_NewBool(ctx, value == "true");
    }
    if (key.find("width") != std::string::npos ||
        key.find("height") != std::string::npos ||
        key.find("overlayToggleKey") != std::string::npos) {
        try { return JS_NewInt32(ctx, std::stoi(value)); }
        catch (...) { return JS_NewString(ctx, value.c_str()); }
    }
    if (key.find("Volume") != std::string::npos ||
        key.find("Speed") != std::string::npos ||
        key.find("Threshold") != std::string::npos ||
        key.find("Distance") != std::string::npos ||
        key.find("Interval") != std::string::npos ||
        key.find("maxFps") != std::string::npos) {
        try { return JS_NewFloat64(ctx, std::stod(value)); }
        catch (...) { return JS_NewString(ctx, value.c_str()); }
    }
    return JS_NewString(ctx, value.c_str());
}

static JSValue buildActionArray(JSContext* ctx,
                                const std::vector<bro::engine::ActionBinding>& actions) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < actions.size(); i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "action",
                          JS_NewString(ctx, actions[i].action.c_str()));
        JSValue keysArr = JS_NewArray(ctx);
        for (size_t j = 0; j < actions[i].keys.size(); j++) {
            JS_SetPropertyUint32(ctx, keysArr, static_cast<uint32_t>(j),
                                 JS_NewString(ctx, actions[i].keys[j].c_str()));
        }
        JS_SetPropertyStr(ctx, obj, "keys", keysArr);
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue js_settings_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    std::string value = state->store->getString(key);
    return settingsValueToJS(ctx, key, value);
}

static JSValue js_settings_get_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string category;
    if (argc >= 1 && JS_IsString(argv[0])) category = jsStr(ctx, argv[0]);

    auto addGraphics = [&](JSValue obj) {
        auto& g = state->store->graphics();
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, g.width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, g.height));
        JS_SetPropertyStr(ctx, obj, "fullscreen", JS_NewBool(ctx, g.fullscreen));
        JS_SetPropertyStr(ctx, obj, "vsync", JS_NewBool(ctx, g.vsync));
        JS_SetPropertyStr(ctx, obj, "resizable", JS_NewBool(ctx, g.resizable));
        JS_SetPropertyStr(ctx, obj, "maxFrameIntervalMs", JS_NewFloat64(ctx, g.maxFrameIntervalMs));
        JS_SetPropertyStr(ctx, obj, "maxFps", JS_NewFloat64(ctx, g.maxFps));
    };
    auto addAudio = [&](JSValue obj) {
        auto& a = state->store->audio();
        JS_SetPropertyStr(ctx, obj, "masterVolume", JS_NewFloat64(ctx, a.masterVolume));
        JS_SetPropertyStr(ctx, obj, "musicVolume", JS_NewFloat64(ctx, a.musicVolume));
        JS_SetPropertyStr(ctx, obj, "sfxVolume", JS_NewFloat64(ctx, a.sfxVolume));
        JS_SetPropertyStr(ctx, obj, "muted", JS_NewBool(ctx, a.muted));
    };
    auto addInput = [&](JSValue obj) {
        auto& i = state->store->input();
        JS_SetPropertyStr(ctx, obj, "scrollSpeed", JS_NewFloat64(ctx, i.scrollSpeed));
        JS_SetPropertyStr(ctx, obj, "doubleClickThresholdMs", JS_NewFloat64(ctx, i.doubleClickThresholdMs));
        JS_SetPropertyStr(ctx, obj, "doubleClickDistancePx", JS_NewFloat64(ctx, i.doubleClickDistancePx));
        JS_SetPropertyStr(ctx, obj, "overlayToggleKey", JS_NewInt32(ctx, static_cast<int32_t>(i.overlayToggleKey)));
    };
    auto addAppearance = [&](JSValue obj) {
        auto& ap = state->store->appearance();
        JS_SetPropertyStr(ctx, obj, "colorScheme", JS_NewString(ctx, ap.colorScheme.c_str()));
    };

    if (category == "appearance") {
        JSValue obj = JS_NewObject(ctx);
        addAppearance(obj);
        return obj;
    }
    if (category == "graphics") {
        JSValue obj = JS_NewObject(ctx);
        addGraphics(obj);
        return obj;
    }
    if (category == "audio") {
        JSValue obj = JS_NewObject(ctx);
        addAudio(obj);
        return obj;
    }
    if (category == "input") {
        JSValue obj = JS_NewObject(ctx);
        addInput(obj);
        return obj;
    }

    JSValue root = JS_NewObject(ctx);
    JSValue gObj = JS_NewObject(ctx); addGraphics(gObj); JS_SetPropertyStr(ctx, root, "graphics", gObj);
    JSValue aObj = JS_NewObject(ctx); addAudio(aObj); JS_SetPropertyStr(ctx, root, "audio", aObj);
    JSValue iObj = JS_NewObject(ctx); addInput(iObj); JS_SetPropertyStr(ctx, root, "input", iObj);
    JSValue apObj = JS_NewObject(ctx); addAppearance(apObj); JS_SetPropertyStr(ctx, root, "appearance", apObj);
    return root;
}

static JSValue js_settings_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    if (JS_IsBool(argv[1])) {
        state->store->setUser(key, JS_ToBool(ctx, argv[1]) != 0);
    } else if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        state->store->setUser(key, d);
    } else {
        state->store->setUser(key, jsStr(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

static JSValue js_settings_set_default(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string key = jsStr(ctx, argv[0]);
    if (JS_IsBool(argv[1])) {
        state->store->setDefault(key, JS_ToBool(ctx, argv[1]) != 0);
    } else if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        state->store->setDefault(key, d);
    } else {
        state->store->setDefault(key, jsStr(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

static JSValue js_settings_reset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    if (argc >= 1 && JS_IsString(argv[0])) {
        state->store->resetCategory(jsStr(ctx, argv[0]));
    } else {
        state->store->resetAll();
    }
    return JS_UNDEFINED;
}

static JSValue js_settings_define_action(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string action = jsStr(ctx, argv[0]);
    std::vector<std::string> keys;
    if (JS_IsArray(argv[1])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], static_cast<uint32_t>(i));
            keys.push_back(jsStr(ctx, elem));
            JS_FreeValue(ctx, elem);
        }
    }
    state->store->defineAction(action, keys);
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue dz = JS_GetPropertyStr(ctx, argv[2], "deadzone");
        if (JS_IsNumber(dz)) {
            double d = 0.0; JS_ToFloat64(ctx, &d, dz);
            state->store->setActionDeadzone(action, static_cast<float>(d));
        }
        JS_FreeValue(ctx, dz);
    }
    return JS_UNDEFINED;
}

static JSValue js_settings_rebind_action(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string action = jsStr(ctx, argv[0]);
    std::vector<std::string> keys;
    if (JS_IsArray(argv[1])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], static_cast<uint32_t>(i));
            keys.push_back(jsStr(ctx, elem));
            JS_FreeValue(ctx, elem);
        }
    }
    state->store->rebindAction(action, keys);
    return JS_UNDEFINED;
}

static JSValue js_settings_reset_action(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    state->store->resetAction(jsStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_settings_reset_all_actions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    state->store->resetAllActions();
    return JS_UNDEFINED;
}

static JSValue js_settings_get_action_keys(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewArray(ctx);
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_NewArray(ctx);
    auto keys = state->store->getKeysForAction(jsStr(ctx, argv[0]));
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < keys.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JS_NewString(ctx, keys[i].c_str()));
    }
    return arr;
}

static JSValue js_settings_get_key_action(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_NULL;
    std::string action = state->store->getActionForKey(jsStr(ctx, argv[0]));
    if (action.empty()) return JS_NULL;
    return JS_NewString(ctx, action.c_str());
}

static JSValue js_settings_get_action_strength(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewFloat64(ctx, 0.0);
    auto* state = getState(ctx);
    if (!state || !state->engine) return JS_NewFloat64(ctx, 0.0);
    float s = state->engine->actionStrength(jsStr(ctx, argv[0]));
    return JS_NewFloat64(ctx, static_cast<double>(s));
}

static JSValue js_settings_is_action_pressed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    auto* state = getState(ctx);
    if (!state || !state->engine) return JS_FALSE;
    return JS_NewBool(ctx, state->engine->actionPressed(jsStr(ctx, argv[0])));
}

static JSValue js_settings_get_actions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_NewArray(ctx);
    return buildActionArray(ctx, state->store->getActions());
}

static JSValue js_settings_get_app_actions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_NewArray(ctx);
    return buildActionArray(ctx, state->store->getAppActions());
}

static JSValue js_settings_get_display_modes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!state->window) return arr;
    auto modes = state->window->getDisplayModes();
    for (size_t i = 0; i < modes.size(); i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, modes[i].width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, modes[i].height));
        JS_SetPropertyStr(ctx, obj, "refreshRate",
                          JS_NewFloat64(ctx, modes[i].refreshRate));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue js_settings_get_defaults(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->store) return JS_UNDEFINED;
    std::string category;
    if (argc >= 1 && JS_IsString(argv[0])) category = jsStr(ctx, argv[0]);

    auto addGraphics = [&](JSValue obj) {
        auto& g = state->store->graphicsDefaults();
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, g.width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, g.height));
        JS_SetPropertyStr(ctx, obj, "fullscreen", JS_NewBool(ctx, g.fullscreen));
        JS_SetPropertyStr(ctx, obj, "vsync", JS_NewBool(ctx, g.vsync));
        JS_SetPropertyStr(ctx, obj, "resizable", JS_NewBool(ctx, g.resizable));
        JS_SetPropertyStr(ctx, obj, "maxFrameIntervalMs", JS_NewFloat64(ctx, g.maxFrameIntervalMs));
        JS_SetPropertyStr(ctx, obj, "maxFps", JS_NewFloat64(ctx, g.maxFps));
    };
    if (category == "graphics") {
        JSValue obj = JS_NewObject(ctx);
        addGraphics(obj);
        return obj;
    }
    JSValue root = JS_NewObject(ctx);
    JSValue gObj = JS_NewObject(ctx);
    addGraphics(gObj);
    JS_SetPropertyStr(ctx, root, "graphics", gObj);
    return root;
}

void SettingsBindings::cleanup(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kSettingsKey);
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        delete reinterpret_cast<SettingsState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_SetPropertyStr(ctx, global, kSettingsKey, JS_UNDEFINED);
    JS_FreeValue(ctx, global);
}


// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void SettingsBindings::install(JSContext* ctx, engine::Settings* settings, platform::Window* window, engine::Engine* engine) {
    auto* state = new SettingsState();
    state->store = settings;
    state->window = window;
    state->engine = engine;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kSettingsKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(state))));

    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue settingsObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, settingsObj, "get",
        JS_NewCFunction(ctx, js_settings_get, "get", 1));
    JS_SetPropertyStr(ctx, settingsObj, "getAll",
        JS_NewCFunction(ctx, js_settings_get_all, "getAll", 1));
    JS_SetPropertyStr(ctx, settingsObj, "set",
        JS_NewCFunction(ctx, js_settings_set, "set", 2));
    JS_SetPropertyStr(ctx, settingsObj, "setDefault",
        JS_NewCFunction(ctx, js_settings_set_default, "setDefault", 2));
    JS_SetPropertyStr(ctx, settingsObj, "reset",
        JS_NewCFunction(ctx, js_settings_reset, "reset", 1));
    JS_SetPropertyStr(ctx, settingsObj, "defineAction",
        JS_NewCFunction(ctx, js_settings_define_action, "defineAction", 3));
    JS_SetPropertyStr(ctx, settingsObj, "rebindAction",
        JS_NewCFunction(ctx, js_settings_rebind_action, "rebindAction", 2));
    JS_SetPropertyStr(ctx, settingsObj, "resetAction",
        JS_NewCFunction(ctx, js_settings_reset_action, "resetAction", 1));
    JS_SetPropertyStr(ctx, settingsObj, "resetAllActions",
        JS_NewCFunction(ctx, js_settings_reset_all_actions, "resetAllActions", 0));
    JS_SetPropertyStr(ctx, settingsObj, "getActionKeys",
        JS_NewCFunction(ctx, js_settings_get_action_keys, "getActionKeys", 1));
    JS_SetPropertyStr(ctx, settingsObj, "getKeyAction",
        JS_NewCFunction(ctx, js_settings_get_key_action, "getKeyAction", 1));
    JS_SetPropertyStr(ctx, settingsObj, "getActionStrength",
        JS_NewCFunction(ctx, js_settings_get_action_strength, "getActionStrength", 1));
    JS_SetPropertyStr(ctx, settingsObj, "isActionPressed",
        JS_NewCFunction(ctx, js_settings_is_action_pressed, "isActionPressed", 1));
    JS_SetPropertyStr(ctx, settingsObj, "getActions",
        JS_NewCFunction(ctx, js_settings_get_actions, "getActions", 0));
    JS_SetPropertyStr(ctx, settingsObj, "getAppActions",
        JS_NewCFunction(ctx, js_settings_get_app_actions, "getAppActions", 0));
    JS_SetPropertyStr(ctx, settingsObj, "getDisplayModes",
        JS_NewCFunction(ctx, js_settings_get_display_modes, "getDisplayModes", 0));
    JS_SetPropertyStr(ctx, settingsObj, "getDefaults",
        JS_NewCFunction(ctx, js_settings_get_defaults, "getDefaults", 1));

    JS_SetPropertyStr(ctx, broObj, "settings", settingsObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js
