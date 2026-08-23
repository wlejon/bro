#if BRO_WITH_TENSOR

#include "gpu_bindings.h"
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

static const char* deviceName(brotensor::Device d) {
    switch (d.type) {
        case brotensor::DeviceType::CUDA:  return "cuda";
        case brotensor::DeviceType::Metal: return "metal";
        case brotensor::DeviceType::CPU:   return "cpu";
    }
    return "cpu";
}

static brotensor::Device parseDeviceArg(JSContext* ctx, int argc, JSValueConst* argv,
                                        int argIdx) {
    if (argc <= argIdx || !JS_IsString(argv[argIdx])) return brotensor::default_device();
    const char* s = JS_ToCString(ctx, argv[argIdx]);
    brotensor::Device d = brotensor::default_device();
    if (s) {
        std::string spec(s);
        JS_FreeCString(ctx, s);
        int index = 0;
        const std::size_t colon = spec.find(':');
        if (colon != std::string::npos) {
            index = std::atoi(spec.c_str() + colon + 1);
            if (index < 0) index = 0;
            spec.resize(colon);
        }
        if (spec == "cuda")       d = brotensor::Device::cuda(index);
        else if (spec == "metal") d = brotensor::Device::metal(index);
        else if (spec == "cpu")   d = brotensor::Device::CPU;
    }
    return d;
}

static bool deviceExists(brotensor::Device want) {
    for (auto d : brotensor::available_devices()) {
        if (d == want) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

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
    const char* last = nullptr;
    for (auto d : brotensor::available_devices()) {
        const char* name = deviceName(d);
        if (last && std::strcmp(last, name) == 0) continue;
        last = name;
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, name));
    }
    return arr;
}

static JSValue js_gpu_get_compiled_backends(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, "cpu"));
    #if defined(BROTENSOR_HAS_CUDA) && BROTENSOR_HAS_CUDA
    JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, "cuda"));
    #endif
    #if defined(BROTENSOR_HAS_METAL) && BROTENSOR_HAS_METAL
    JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, "metal"));
    #endif
    return arr;
}

static JSValue js_gpu_device_count(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device want = parseDeviceArg(ctx, argc, argv, 0);
    int n = 0;
    for (auto d : brotensor::available_devices()) {
        if (d.type == want.type) ++n;
    }
    return JS_NewInt32(ctx, n);
}

static JSValue js_gpu_memory_info(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    if (!deviceExists(d)) return JS_NULL;
    std::size_t freeBytes = 0, totalBytes = 0;
    if (!brotensor::device_mem_info(d, freeBytes, totalBytes)) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "freeBytes", JS_NewInt64(ctx, static_cast<int64_t>(freeBytes)));
    JS_SetPropertyStr(ctx, o, "totalBytes", JS_NewInt64(ctx, static_cast<int64_t>(totalBytes)));
    return o;
}

static JSValue js_gpu_device_name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    if (!deviceExists(d)) return JS_NULL;
    std::string name = brotensor::device_product_name(d);
    if (name.empty()) return JS_NULL;
    return JS_NewString(ctx, name.c_str());
}

static JSValue js_gpu_trim(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brotensor::init();
    brotensor::Device d = parseDeviceArg(ctx, argc, argv, 0);
    if (!deviceExists(d)) return JS_NewBool(ctx, false);
    std::size_t keepBytes = 0;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int64_t v = 0; JS_ToInt64(ctx, &v, argv[1]);
        if (v > 0) keepBytes = static_cast<std::size_t>(v);
    }
    return JS_NewBool(ctx, brotensor::device_mem_trim(d, keepBytes));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installGpuBindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue gpuObj = JS_NewObject(ctx);

    auto defineGetSet = [&](const char* name, JSCFunction* getter,
                            JSCFunction* setter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, gpuObj, atom,
            JS_NewCFunction(ctx, getter, name, 0),
            setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetSet("available",  js_gpu_get_available,  nullptr);
    defineGetSet("backend",  js_gpu_get_backend,  nullptr);
    defineGetSet("devices",  js_gpu_get_devices,  nullptr);
    defineGetSet("compiledBackends",  js_gpu_get_compiled_backends,  nullptr);
    JS_SetPropertyStr(ctx, gpuObj, "deviceCount",
        JS_NewCFunction(ctx, js_gpu_device_count, "deviceCount", 1));
    JS_SetPropertyStr(ctx, gpuObj, "memoryInfo",
        JS_NewCFunction(ctx, js_gpu_memory_info, "memoryInfo", 1));
    JS_SetPropertyStr(ctx, gpuObj, "deviceName",
        JS_NewCFunction(ctx, js_gpu_device_name, "deviceName", 1));
    JS_SetPropertyStr(ctx, gpuObj, "trim",
        JS_NewCFunction(ctx, js_gpu_trim, "trim", 2));

    JS_SetPropertyStr(ctx, broObj, "gpu", gpuObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js

#endif // BRO_WITH_TENSOR
