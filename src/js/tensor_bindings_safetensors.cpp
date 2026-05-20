// JS bindings — safetensors load / save, on bro.tensor.
//
//   bro.tensor.openSafetensors(path) -> handle
//       handle.count                 number of tensors
//       handle.names()               -> [string, ...]
//       handle.header()              -> { name: {dtype, shape:[...], nbytes}, ... }
//                                       (metadata only — no payload read)
//       handle.get(name, rows?, cols?) -> GpuTensor  (F16/F32 source only)
//       handle.close()               release the mmap early (also freed on GC)
//
//   bro.tensor.saveSafetensors(path, { name: GpuTensor, ... })
//
// The handle owns the mmap'd brotensor::safetensors::File; TensorView data
// pointers stay valid only while the handle is open. See tensor_bindings.cpp
// for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

#include <brotensor/safetensors.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

namespace stns = brotensor::safetensors;

// Opaque handle: owns the mmap'd File. `file` is null after close(); the
// qjsbind finalizer deletes this struct (and so unmaps) on GC.
struct SafetensorsFileData {
    std::unique_ptr<stns::File> file;
};

// ─── openSafetensors / saveSafetensors (free functions) ───────────────────

static JSValue js_openSafetensors(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "openSafetensors(path) — expected a string path");
    const char* cpath = JS_ToCString(ctx, argv[0]);
    if (!cpath) return JS_EXCEPTION;
    std::string path(cpath);
    JS_FreeCString(ctx, cpath);

    auto* d = new SafetensorsFileData();
    try {
        d->file = std::make_unique<stns::File>(stns::File::open(path));
    } catch (const std::exception& e) {
        delete d;
        std::string msg = std::string("openSafetensors: ") + e.what();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    return qjsbind::wrap<SafetensorsFileData>(ctx, d);
}

static JSValue js_saveSafetensors(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "saveSafetensors(path, {name: tensor, ...})");
    const char* cpath = JS_ToCString(ctx, argv[0]);
    if (!cpath) return JS_EXCEPTION;
    std::string path(cpath);
    JS_FreeCString(ctx, cpath);

    BROTENSOR_ENSURE_INIT();
    nngpu::sync_all();

    JSPropertyEnum* tab = nullptr;
    uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &plen, argv[1],
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return JS_ThrowTypeError(ctx, "saveSafetensors: cannot enumerate tensors");

    std::vector<stns::WriteEntry> entries;
    // Backing host buffers — kept alive until write_file() returns. reserve()
    // so the inner vectors are never reallocated out from under host_data.
    std::vector<std::vector<float>> f32store;
    std::vector<std::vector<uint16_t>> f16store;
    entries.reserve(plen);
    f32store.reserve(plen);
    f16store.reserve(plen);

    JSValue err = JS_UNDEFINED;
    bool failed = false;
    for (uint32_t i = 0; i < plen && !failed; ++i) {
        const char* key = JS_AtomToCString(ctx, tab[i].atom);
        JSValue val = JS_GetProperty(ctx, argv[1], tab[i].atom);
        auto* gt = gpuTensorFromJSLocal(ctx, val);
        if (!gt) {
            std::string msg = std::string("saveSafetensors: value for '") +
                              (key ? key : "?") + "' is not a tensor";
            err = JS_ThrowTypeError(ctx, "%s", msg.c_str());
            failed = true;
        } else {
            // gpuTensorFromJSLocal returns the brotensor::Tensor* directly.
            stns::WriteEntry e;
            e.name  = key ? key : "";
            e.shape = { gt->rows, gt->cols };
            nngpu::Tensor host = gt->to(nngpu::Device::CPU);
            if (host.dtype == nngpu::Dtype::FP32) {
                f32store.push_back(host.to_host_vector());
                e.dtype     = stns::Dtype::F32;
                e.host_data = f32store.back().data();
                e.bytes     = f32store.back().size() * sizeof(float);
                entries.push_back(std::move(e));
            } else if (host.dtype == nngpu::Dtype::FP16) {
                f16store.push_back(host.to_host_vector_fp16());
                e.dtype     = stns::Dtype::F16;
                e.host_data = f16store.back().data();
                e.bytes     = f16store.back().size() * sizeof(uint16_t);
                entries.push_back(std::move(e));
            } else {
                std::string msg = std::string("saveSafetensors: '") +
                    (key ? key : "?") + "' has an unsupported dtype (FP32/FP16 only)";
                err = JS_ThrowTypeError(ctx, "%s", msg.c_str());
                failed = true;
            }
        }
        if (key) JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, val);
    }
    for (uint32_t i = 0; i < plen; ++i) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    if (failed) return err;

    try {
        stns::write_file(path, entries);
    } catch (const std::exception& e) {
        std::string msg = std::string("saveSafetensors: ") + e.what();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    return JS_UNDEFINED;
}

// ─── handle methods ───────────────────────────────────────────────────────

static JSValue js_st_names(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<SafetensorsFileData>(ctx, this_val);
    if (!d || !d->file) return JS_ThrowTypeError(ctx, "names() on a closed safetensors file");
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& tv : d->file->tensors()) {
        JS_SetPropertyUint32(ctx, arr, i++,
            JS_NewStringLen(ctx, tv.name.data(), tv.name.size()));
    }
    return arr;
}

// header() -> { name: { dtype, shape:[...], nbytes }, ... }. Metadata only —
// the payload is never touched, so this is cheap even on a multi-GB file.
static JSValue js_st_header(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<SafetensorsFileData>(ctx, this_val);
    if (!d || !d->file) return JS_ThrowTypeError(ctx, "header() on a closed safetensors file");
    JSValue obj = JS_NewObject(ctx);
    for (const auto& tv : d->file->tensors()) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "dtype", JS_NewString(ctx, stns::dtype_name(tv.dtype)));
        JSValue shape = JS_NewArray(ctx);
        for (uint32_t j = 0; j < tv.shape.size(); ++j)
            JS_SetPropertyUint32(ctx, shape, j, JS_NewInt64(ctx, tv.shape[j]));
        JS_SetPropertyStr(ctx, entry, "shape", shape);
        JS_SetPropertyStr(ctx, entry, "nbytes",
            JS_NewInt64(ctx, static_cast<int64_t>(tv.nbytes)));
        JS_SetPropertyStr(ctx, obj, tv.name.c_str(), entry);
    }
    return obj;
}

// get(name, rows?, cols?) -> GpuTensor. brotensor tensors are 2D; when rows/
// cols are omitted the N-D source is flattened to (shape[0], numel/shape[0]).
// Source dtype must be F16 or F32 (brotensor::safetensors::upload's contract).
static JSValue js_st_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SafetensorsFileData>(ctx, this_val);
    if (!d || !d->file) return JS_ThrowTypeError(ctx, "get() on a closed safetensors file");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "get(name, rows?, cols?)");
    const char* cname = JS_ToCString(ctx, argv[0]);
    if (!cname) return JS_EXCEPTION;
    std::string name(cname);
    JS_FreeCString(ctx, cname);

    const stns::TensorView* tv = d->file->find(name);
    if (!tv) {
        std::string msg = "get: no tensor named '" + name + "'";
        return JS_ThrowRangeError(ctx, "%s", msg.c_str());
    }

    int32_t rows = 0, cols = 0;
    if (argc >= 3 && !JS_IsUndefined(argv[1]) && !JS_IsUndefined(argv[2])) {
        JS_ToInt32(ctx, &rows, argv[1]);
        JS_ToInt32(ctx, &cols, argv[2]);
    } else {
        try {
            int64_t numel = tv->numel();
            rows = tv->shape.empty() ? 1 : static_cast<int32_t>(tv->shape[0]);
            cols = rows > 0 ? static_cast<int32_t>(numel / rows) : 0;
        } catch (const std::exception& e) {
            std::string msg = std::string("get: ") + e.what();
            return JS_ThrowTypeError(ctx, "%s", msg.c_str());
        }
    }

    BROTENSOR_ENSURE_INIT();
    auto* gt = new GpuTensorData();
    try {
        stns::upload(*tv, rows, cols, gt->t);
    } catch (const std::exception& e) {
        delete gt;
        std::string msg = std::string("get: ") + e.what();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    return qjsbind::wrap<GpuTensorData>(ctx, gt);
}

// ─── install ──────────────────────────────────────────────────────────────

void installTensorSafetensorsOps(JSContext* ctx, JSValue gpuObj) {
    qjsbind::Class<SafetensorsFileData>(ctx, "SafetensorsFile", qjsbind::NoGlobal)
        .get("count", [](SafetensorsFileData* d) -> int {
            return d->file ? static_cast<int>(d->file->size()) : 0;
        })
        .method("close", [](SafetensorsFileData* d) { d->file.reset(); })
        .method_raw("names",  js_st_names,  0)
        .method_raw("header", js_st_header, 0)
        .method_raw("get",    js_st_get,    3);

    JS_SetPropertyStr(ctx, gpuObj, "openSafetensors",
        JS_NewCFunction(ctx, js_openSafetensors, "openSafetensors", 1));
    JS_SetPropertyStr(ctx, gpuObj, "saveSafetensors",
        JS_NewCFunction(ctx, js_saveSafetensors, "saveSafetensors", 2));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorSafetensorsOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
