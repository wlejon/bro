// Installed onto bro.gpu by installGpuBindings(). A thin, always-present
// runtime probe over brotensor's registered backends — see gpu_bindings.h for
// the rationale (it exists even in CPU-only builds, where bro.tensor stubs out).
//
// The getters call brotensor::init() lazily: it is idempotent and the result is
// stable once the CUDA / Metal drivers have been probed, so a non-ML app that
// never reads bro.gpu pays nothing at context creation. `backend`/`available`
// report brotensor::default_device() — the canonical "best available" device
// (CUDA > Metal > CPU) that governs where the next tensor, and therefore a
// freshly-loaded model, lands.

#include "gpu_bindings.h"

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

namespace bro::js {

static const char* deviceName(brotensor::Device d) {
    switch (d) {
        case brotensor::Device::CUDA:  return "cuda";
        case brotensor::Device::Metal: return "metal";
        case brotensor::Device::CPU:   return "cpu";
    }
    return "cpu";
}

static JSValue js_gpu_get_available(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    brotensor::init();
    return JS_NewBool(ctx, brotensor::default_device() != brotensor::Device::CPU);
}

static JSValue js_gpu_get_backend(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    brotensor::init();
    return JS_NewString(ctx, deviceName(brotensor::default_device()));
}

static JSValue js_gpu_get_devices(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    brotensor::init();
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto d : brotensor::available_devices()) {
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, deviceName(d)));
    }
    return arr;
}

static void defineGetter(JSContext* ctx, JSValue obj, const char* name,
                         JSCFunction* getter) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, obj, atom,
        JS_NewCFunction(ctx, getter, name, 0),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

void installGpuBindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue gpuObj = JS_NewObject(ctx);
    defineGetter(ctx, gpuObj, "available", js_gpu_get_available);
    defineGetter(ctx, gpuObj, "backend",   js_gpu_get_backend);
    defineGetter(ctx, gpuObj, "devices",   js_gpu_get_devices);
    JS_SetPropertyStr(ctx, broObj, "gpu", gpuObj);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
