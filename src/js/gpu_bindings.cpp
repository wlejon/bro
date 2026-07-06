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

#include <cstring>

namespace bro::js {

static const char* deviceName(brotensor::Device d) {
    switch (d) {
        case brotensor::Device::CUDA:  return "cuda";
        case brotensor::Device::Metal: return "metal";
        case brotensor::Device::CPU:   return "cpu";
    }
    return "cpu";
}

// Parses argv[argIdx] as one of 'cuda'/'metal'/'cpu'; falls back to
// default_device() when the arg is missing or not a string.
static brotensor::Device parseDeviceArg(JSContext* ctx, int argc, JSValueConst* argv,
                                        int argIdx) {
    if (argc <= argIdx || !JS_IsString(argv[argIdx])) return brotensor::default_device();
    const char* s = JS_ToCString(ctx, argv[argIdx]);
    brotensor::Device d = brotensor::default_device();
    if (s) {
        if (std::strcmp(s, "cuda") == 0)  d = brotensor::Device::CUDA;
        else if (std::strcmp(s, "metal") == 0) d = brotensor::Device::Metal;
        else if (std::strcmp(s, "cpu") == 0)   d = brotensor::Device::CPU;
        JS_FreeCString(ctx, s);
    }
    return d;
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

// memoryInfo(device?) -> {freeBytes, totalBytes} | null. device is
// 'cuda'/'metal'/'cpu', defaulting to the current default device. Returns
// null when the backend isn't registered or can't report (always null on CPU).
static JSValue js_gpu_memoryInfo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    std::size_t freeBytes = 0, totalBytes = 0;
    if (!brotensor::device_mem_info(d, freeBytes, totalBytes)) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "freeBytes", JS_NewInt64(ctx, static_cast<int64_t>(freeBytes)));
    JS_SetPropertyStr(ctx, o, "totalBytes", JS_NewInt64(ctx, static_cast<int64_t>(totalBytes)));
    return o;
}

// deviceName(device?) -> string | null. The card's human-readable name
// (cudaDeviceProp.name, e.g. "NVIDIA GeForce RTX 4090"), for `device`
// ('cuda'/'metal'/'cpu', defaulting to the current default device). Null when
// the backend isn't registered or can't report (always null on CPU).
static JSValue js_gpu_deviceName(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    std::string name = brotensor::device_product_name(d);
    if (name.empty()) return JS_NULL;
    return JS_NewString(ctx, name.c_str());
}

// trim(device?, keepBytes=0) -> boolean. Returns the backend allocator's
// cached-but-unused memory to the driver, keeping at most keepBytes cached.
// Use between pipeline phases with very different scratch shapes — cached
// blocks count against device residency, and on Windows (WDDM) sustained
// near-full commit silently demotes large resident allocations to shared
// memory, turning weight reads into PCIe traffic. False when the backend
// isn't registered or has no trimmable allocator (always false on CPU).
static JSValue js_gpu_trim(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    std::size_t keepBytes = 0;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int64_t v = 0; JS_ToInt64(ctx, &v, argv[1]);
        if (v > 0) keepBytes = static_cast<std::size_t>(v);
    }
    return JS_NewBool(ctx, brotensor::device_mem_trim(d, keepBytes));
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
    JS_SetPropertyStr(ctx, gpuObj, "memoryInfo",
                      JS_NewCFunction(ctx, js_gpu_memoryInfo, "memoryInfo", 1));
    JS_SetPropertyStr(ctx, gpuObj, "deviceName",
                      JS_NewCFunction(ctx, js_gpu_deviceName, "deviceName", 1));
    JS_SetPropertyStr(ctx, gpuObj, "trim",
                      JS_NewCFunction(ctx, js_gpu_trim, "trim", 2));
    JS_SetPropertyStr(ctx, broObj, "gpu", gpuObj);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
