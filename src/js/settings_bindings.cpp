#include "js/settings_bindings.h"
#include "engine/settings.h"
#include "platform/sdl_window.h"
#include "util/log.h"

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// State pointers stashed in JS globals
// ---------------------------------------------------------------------------

static const char* kSettingsKey = "__bro_settings_ptr";
static const char* kWindowKey = "__bro_settings_window_ptr";

struct SettingsState {
    engine::Settings* settings = nullptr;
    platform::Window* window = nullptr;
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

// ---------------------------------------------------------------------------
// JS helper
// ---------------------------------------------------------------------------

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

// Convert a settings value string to the appropriate JS type
static JSValue settingsValueToJS(JSContext* ctx, const std::string& key,
                                 const std::string& value) {
    if (value.empty()) return JS_UNDEFINED;

    // Boolean fields
    if (key.find("fullscreen") != std::string::npos ||
        key.find("vsync") != std::string::npos ||
        key.find("resizable") != std::string::npos ||
        key.find("muted") != std::string::npos) {
        return JS_NewBool(ctx, value == "true");
    }

    // Integer fields
    if (key.find("width") != std::string::npos ||
        key.find("height") != std::string::npos ||
        key.find("overlayToggleKey") != std::string::npos) {
        try { return JS_NewInt32(ctx, std::stoi(value)); }
        catch (...) { return JS_NewString(ctx, value.c_str()); }
    }

    // Float/double fields
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

// ---------------------------------------------------------------------------
// bro.settings.get(key)
// ---------------------------------------------------------------------------

static JSValue js_settings_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string key = jsStr(ctx, argv[0]);
    std::string value = state->settings->getString(key);
    return settingsValueToJS(ctx, key, value);
}

// ---------------------------------------------------------------------------
// bro.settings.getAll([category])
// ---------------------------------------------------------------------------

static JSValue js_settings_getAll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string category;
    if (argc >= 1 && JS_IsString(argv[0]))
        category = jsStr(ctx, argv[0]);

    auto addGraphics = [&](JSValue obj) {
        auto& g = state->settings->graphics();
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, g.width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, g.height));
        JS_SetPropertyStr(ctx, obj, "fullscreen", JS_NewBool(ctx, g.fullscreen));
        JS_SetPropertyStr(ctx, obj, "vsync", JS_NewBool(ctx, g.vsync));
        JS_SetPropertyStr(ctx, obj, "resizable", JS_NewBool(ctx, g.resizable));
        JS_SetPropertyStr(ctx, obj, "maxFrameIntervalMs", JS_NewFloat64(ctx, g.maxFrameIntervalMs));
        JS_SetPropertyStr(ctx, obj, "maxFps", JS_NewFloat64(ctx, g.maxFps));
    };

    auto addAudio = [&](JSValue obj) {
        auto& a = state->settings->audio();
        JS_SetPropertyStr(ctx, obj, "masterVolume", JS_NewFloat64(ctx, a.masterVolume));
        JS_SetPropertyStr(ctx, obj, "musicVolume", JS_NewFloat64(ctx, a.musicVolume));
        JS_SetPropertyStr(ctx, obj, "sfxVolume", JS_NewFloat64(ctx, a.sfxVolume));
        JS_SetPropertyStr(ctx, obj, "muted", JS_NewBool(ctx, a.muted));
    };

    auto addInput = [&](JSValue obj) {
        auto& i = state->settings->input();
        JS_SetPropertyStr(ctx, obj, "scrollSpeed", JS_NewFloat64(ctx, i.scrollSpeed));
        JS_SetPropertyStr(ctx, obj, "doubleClickThresholdMs", JS_NewFloat64(ctx, i.doubleClickThresholdMs));
        JS_SetPropertyStr(ctx, obj, "doubleClickDistancePx", JS_NewFloat64(ctx, i.doubleClickDistancePx));
        JS_SetPropertyStr(ctx, obj, "overlayToggleKey", JS_NewInt32(ctx, static_cast<int32_t>(i.overlayToggleKey)));
    };

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

    // No category — return all
    JSValue root = JS_NewObject(ctx);
    JSValue gObj = JS_NewObject(ctx);
    addGraphics(gObj);
    JS_SetPropertyStr(ctx, root, "graphics", gObj);

    JSValue aObj = JS_NewObject(ctx);
    addAudio(aObj);
    JS_SetPropertyStr(ctx, root, "audio", aObj);

    JSValue iObj = JS_NewObject(ctx);
    addInput(iObj);
    JS_SetPropertyStr(ctx, root, "input", iObj);

    return root;
}

// ---------------------------------------------------------------------------
// bro.settings.set(key, value)
// ---------------------------------------------------------------------------

static JSValue js_settings_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string key = jsStr(ctx, argv[0]);

    if (JS_IsBool(argv[1])) {
        state->settings->setUser(key, JS_ToBool(ctx, argv[1]) != 0);
    } else if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        state->settings->setUser(key, d);
    } else {
        state->settings->setUser(key, jsStr(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.setDefault(key, value)
// ---------------------------------------------------------------------------

static JSValue js_settings_setDefault(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string key = jsStr(ctx, argv[0]);

    if (JS_IsBool(argv[1])) {
        state->settings->setDefault(key, JS_ToBool(ctx, argv[1]) != 0);
    } else if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        state->settings->setDefault(key, d);
    } else {
        state->settings->setDefault(key, jsStr(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.reset([category])
// ---------------------------------------------------------------------------

static JSValue js_settings_reset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    if (argc >= 1 && JS_IsString(argv[0])) {
        state->settings->resetCategory(jsStr(ctx, argv[0]));
    } else {
        state->settings->resetAll();
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.defineAction(action, keys[])
// ---------------------------------------------------------------------------

static JSValue js_settings_defineAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string action = jsStr(ctx, argv[0]);
    std::vector<std::string> keys;

    if (JS_IsArray(argv[1])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], static_cast<uint32_t>(i));
            keys.push_back(jsStr(ctx, elem));
            JS_FreeValue(ctx, elem);
        }
    }

    state->settings->defineAction(action, keys);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.rebindAction(action, keys[])
// ---------------------------------------------------------------------------

static JSValue js_settings_rebindAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string action = jsStr(ctx, argv[0]);
    std::vector<std::string> keys;

    if (JS_IsArray(argv[1])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], static_cast<uint32_t>(i));
            keys.push_back(jsStr(ctx, elem));
            JS_FreeValue(ctx, elem);
        }
    }

    state->settings->rebindAction(action, keys);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.resetAction(action)
// ---------------------------------------------------------------------------

static JSValue js_settings_resetAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;
    state->settings->resetAction(jsStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.resetAllActions()
// ---------------------------------------------------------------------------

static JSValue js_settings_resetAllActions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;
    state->settings->resetAllActions();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.settings.getActionKeys(action)
// ---------------------------------------------------------------------------

static JSValue js_settings_getActionKeys(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewArray(ctx);
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_NewArray(ctx);

    auto keys = state->settings->getKeysForAction(jsStr(ctx, argv[0]));
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < keys.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, keys[i].c_str()));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// bro.settings.getKeyAction(key)
// ---------------------------------------------------------------------------

static JSValue js_settings_getKeyAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_NULL;

    std::string action = state->settings->getActionForKey(jsStr(ctx, argv[0]));
    if (action.empty()) return JS_NULL;
    return JS_NewString(ctx, action.c_str());
}

// ---------------------------------------------------------------------------
// bro.settings.getActions()
// ---------------------------------------------------------------------------

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

static JSValue js_settings_getActions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_NewArray(ctx);
    return buildActionArray(ctx, state->settings->getActions());
}

static JSValue js_settings_getAppActions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_NewArray(ctx);
    return buildActionArray(ctx, state->settings->getAppActions());
}

// ---------------------------------------------------------------------------
// bro.settings.getDefaults(category?)
// Returns the engine/app default values (bro.json layer), independent of
// user overrides. Useful for surfacing the "app preferred" resolution.
// ---------------------------------------------------------------------------

static JSValue js_settings_getDefaults(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* state = getState(ctx);
    if (!state || !state->settings) return JS_UNDEFINED;

    std::string category;
    if (argc >= 1 && JS_IsString(argv[0]))
        category = jsStr(ctx, argv[0]);

    auto addGraphics = [&](JSValue obj) {
        auto& g = state->settings->graphicsDefaults();
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

// ---------------------------------------------------------------------------
// bro.settings.getDisplayModes()
// ---------------------------------------------------------------------------

static JSValue js_settings_getDisplayModes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
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

// ---------------------------------------------------------------------------
// Function list for bro.settings namespace
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_settings_funcs[] = {
    JS_CFUNC_DEF("get", 1, js_settings_get),
    JS_CFUNC_DEF("getAll", 0, js_settings_getAll),
    JS_CFUNC_DEF("set", 2, js_settings_set),
    JS_CFUNC_DEF("setDefault", 2, js_settings_setDefault),
    JS_CFUNC_DEF("reset", 0, js_settings_reset),
    JS_CFUNC_DEF("defineAction", 2, js_settings_defineAction),
    JS_CFUNC_DEF("rebindAction", 2, js_settings_rebindAction),
    JS_CFUNC_DEF("resetAction", 1, js_settings_resetAction),
    JS_CFUNC_DEF("resetAllActions", 0, js_settings_resetAllActions),
    JS_CFUNC_DEF("getActionKeys", 1, js_settings_getActionKeys),
    JS_CFUNC_DEF("getKeyAction", 1, js_settings_getKeyAction),
    JS_CFUNC_DEF("getActions", 0, js_settings_getActions),
    JS_CFUNC_DEF("getAppActions", 0, js_settings_getAppActions),
    JS_CFUNC_DEF("getDisplayModes", 0, js_settings_getDisplayModes),
    JS_CFUNC_DEF("getDefaults", 0, js_settings_getDefaults),
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void SettingsBindings::install(JSContext* ctx, engine::Settings* settings,
                               platform::Window* window) {
    auto* state = new SettingsState();
    state->settings = settings;
    state->window = window;

    JSValue global = JS_GetGlobalObject(ctx);

    // Stash pointer
    JS_SetPropertyStr(ctx, global, kSettingsKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(state))));

    // Create bro.settings namespace
    JSValue bro = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(bro) || JS_IsNull(bro)) {
        JS_FreeValue(ctx, bro);
        bro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, bro));
    }

    JSValue settingsObj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, settingsObj, js_settings_funcs,
                               sizeof(js_settings_funcs) / sizeof(js_settings_funcs[0]));
    JS_SetPropertyStr(ctx, bro, "settings", settingsObj);

    JS_FreeValue(ctx, bro);
    JS_FreeValue(ctx, global);
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

} // namespace bro::js
