#include "js/gamepad_bindings.h"
#include "engine/engine.h"
#include "engine/gamepad.h"
#include "util/log.h"
#include <string>

namespace bro::js {

static const char* kGamepadEngineKey = "__bro_gamepad_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kGamepadEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

static JSValue resolvedPromise(JSContext* ctx, JSValue value) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, value);
    return promise;
}

static JSValue js_playEffect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv,
                             int, JSValue* data) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");
    int32_t index = 0;
    JS_ToInt32(ctx, &index, data[0]);

    bool triggerRumble = false;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* type = JS_ToCString(ctx, argv[0]);
        std::string t = type ? type : "";
        if (type) JS_FreeCString(ctx, type);
        triggerRumble = (t == "trigger-rumble");
        if (!triggerRumble && t != "dual-rumble") {
            return JS_ThrowTypeError(ctx,
                "playEffect: only \"dual-rumble\" and \"trigger-rumble\" are supported");
        }
    }

    double duration = 0.0, strong = 0.0, weak = 0.0;
    double leftTrigger = 0.0, rightTrigger = 0.0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "duration");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &duration, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "strongMagnitude");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &strong, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "weakMagnitude");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &weak, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "leftTrigger");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &leftTrigger, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "rightTrigger");
        if (JS_IsNumber(v)) JS_ToFloat64(ctx, &rightTrigger, v);
        JS_FreeValue(ctx, v);
    }

    bool ok = engine->gamepadRumble(index, static_cast<float>(strong),
                                    static_cast<float>(weak),
                                    static_cast<int>(duration));
    if (triggerRumble) {
        ok = engine->gamepadRumbleTriggers(index,
                                           static_cast<float>(leftTrigger),
                                           static_cast<float>(rightTrigger),
                                           static_cast<int>(duration)) && ok;
    }
    return resolvedPromise(ctx, JS_NewString(ctx, ok ? "complete" : "preempted"));
}

static JSValue js_resetEffect(JSContext* ctx, JSValueConst, int, JSValueConst*,
                              int, JSValue* data) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");
    int32_t index = 0;
    JS_ToInt32(ctx, &index, data[0]);
    engine->gamepadRumble(index, 0.0f, 0.0f, 0);
    engine->gamepadRumbleTriggers(index, 0.0f, 0.0f, 0);
    return resolvedPromise(ctx, JS_NewString(ctx, "complete"));
}

JSValue buildGamepadSnapshot(JSContext* ctx, engine::Engine*, const engine::GamepadState& gp) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, gp.id.c_str()));
    JS_SetPropertyStr(ctx, obj, "index", JS_NewInt32(ctx, gp.index));
    JS_SetPropertyStr(ctx, obj, "connected", JS_NewBool(ctx, gp.connected));
    JS_SetPropertyStr(ctx, obj, "mapping", JS_NewString(ctx, "standard"));
    JS_SetPropertyStr(ctx, obj, "timestamp", JS_NewFloat64(ctx, gp.timestampMs));

    JSValue buttons = JS_NewArray(ctx);
    for (int i = 0; i < engine::kGamepadButtonCount; i++) {
        float value = gp.buttons[i];
        bool pressed = value >= engine::kGamepadTriggerPressThreshold;
        JSValue b = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, b, "pressed", JS_NewBool(ctx, pressed));
        JS_SetPropertyStr(ctx, b, "touched", JS_NewBool(ctx, pressed || value > 0.0f));
        JS_SetPropertyStr(ctx, b, "value", JS_NewFloat64(ctx, value));
        JS_SetPropertyUint32(ctx, buttons, static_cast<uint32_t>(i), b);
    }
    JS_SetPropertyStr(ctx, obj, "buttons", buttons);

    JSValue axes = JS_NewArray(ctx);
    for (int i = 0; i < engine::kGamepadAxisCount; i++) {
        JS_SetPropertyUint32(ctx, axes, static_cast<uint32_t>(i),
                             JS_NewFloat64(ctx, gp.axes[i]));
    }
    JS_SetPropertyStr(ctx, obj, "axes", axes);

    JSValue actuator = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, actuator, "type", JS_NewString(ctx, "dual-rumble"));
    {
        JSValue effects = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, effects, 0, JS_NewString(ctx, "dual-rumble"));
        JS_SetPropertyUint32(ctx, effects, 1, JS_NewString(ctx, "trigger-rumble"));
        JS_SetPropertyStr(ctx, actuator, "effects", effects);
    }
    JSValue indexVal = JS_NewInt32(ctx, gp.index);
    JS_SetPropertyStr(ctx, actuator, "playEffect",
        JS_NewCFunctionData(ctx, js_playEffect, 2, 0, 1, &indexVal));
    JS_SetPropertyStr(ctx, actuator, "reset",
        JS_NewCFunctionData(ctx, js_resetEffect, 0, 0, 1, &indexVal));
    JS_FreeValue(ctx, indexVal);
    JS_SetPropertyStr(ctx, obj, "vibrationActuator", actuator);

    return obj;
}

static JSValue js_getGamepads(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!engine) return arr;
    const auto& pads = engine->gamepads();
    for (size_t i = 0; i < pads.size(); i++) {
        JSValue entry = pads[i].connected
            ? buildGamepadSnapshot(ctx, engine, pads[i])
            : JS_NULL;
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), entry);
    }
    return arr;
}

void installGamepadBindings(JSContext* ctx, engine::Engine* engine)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kGamepadEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    JSValue nav = JS_GetPropertyStr(ctx, global, "navigator");
    if (JS_IsUndefined(nav) || JS_IsNull(nav)) {
        JS_FreeValue(ctx, nav);
        nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "navigator", JS_DupValue(ctx, nav));
    }
    JS_SetPropertyStr(ctx, nav, "getGamepads",
                      JS_NewCFunction(ctx, js_getGamepads, "getGamepads", 0));
    JS_FreeValue(ctx, nav);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js
