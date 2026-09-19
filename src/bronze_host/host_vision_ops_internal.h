#pragma once

// Shared by the four files that re-drive brovisionml's ops on bro's job
// machine (host_vision_ops.cpp, host_vision_sam.cpp,
// host_vision_generative.cpp, host_vision_annotators.cpp).
//
// The model wrappers, the image reader, the typed-array makers and
// buildSegmentation all come from brovisionml's own api header — reached by
// its plain name because brovisionml_api puts src/api on the include path.
// Pulling them in rather than re-deriving them is the point: the model call
// and the result shape keep ONE definition, and what bro adds on top is the
// ImageBitmap and the thread.

#if BRO_WITH_VISION

#include "bronze_host/host_vision.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder

// brovisionml/src/api/host_vision_internal.h — the wrapper structs, the tags,
// the HostClass handles, readImageInput and the make*Array helpers.
#include "host_vision_internal.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace bvm = brovisionml::api;

// The brovisionml wrapper behind `thisVal`, or nullptr when the value is not
// an instance of that class. Same tag check the sibling's own handlers do.
template <typename WrapperT>
WrapperT* visionSelf(const bvm::HostClass& cls, Value thisVal, uint32_t tag) {
    void* p = cls.unwrap(thisVal);
    if (!p) return nullptr;
    auto* w = static_cast<WrapperT*>(p);
    return w->tag == tag ? w : nullptr;
}

// Option readers. Every one leaves `dst` alone for a missing or wrongly
// typed key, so a caller's partial option bag means "the rest as before".
inline void visionIntOpt(Value opts, const char* key, int& dst) {
    if (!ev::isObject(opts)) return;
    Value v = ev::getProperty(opts, key);
    if (ev::isNumber(v)) dst = static_cast<int>(ev::toDouble(v));
}

inline void visionFloatOpt(Value opts, const char* key, float& dst) {
    if (!ev::isObject(opts)) return;
    Value v = ev::getProperty(opts, key);
    if (ev::isNumber(v)) dst = static_cast<float>(ev::toDouble(v));
}

inline bool visionBoolOpt(Value opts, const char* key, bool def) {
    if (!ev::isObject(opts)) return def;
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toBool(v);
}

inline void visionInt64Opt(Value opts, const char* key, int64_t& dst) {
    if (!ev::isObject(opts)) return;
    Value v = ev::getProperty(opts, key);
    if (ev::isNumber(v)) dst = static_cast<int64_t>(ev::toDouble(v));
}

// A Float32Array-valued option into a host vector. False when the key is
// absent or is not a typed array.
inline bool visionFloatsOpt(Value opts, const char* key, std::vector<float>& out) {
    if (!ev::isObject(opts)) return false;
    Value v = ev::getProperty(opts, key);
    const float* data = nullptr;
    std::size_t count = 0;
    if (!bvm::readFloat32Array(v, data, count) || !data) return false;
    out.assign(data, data + count);
    return true;
}

// The two-line guard every wrapped op opens with: refuse a second op while
// one is in flight on the same model, the way the old binding's `busy` flag
// did. The worker reads the model's own workspace, so overlapping calls on
// one model are two threads in one brotensor arena.
#define BRO_VISION_BEGIN(modelPtr, opName)                                     \
    if (!visionMarkBusy(modelPtr)) {                                           \
        return ev::throwError(opName                                           \
                              ": an operation is already in flight on this model"); \
    }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
