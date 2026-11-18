#pragma once

// Building and reading plain values: real arrays, typed arrays in and out,
// DOMRect-shaped objects, the Error a DOMException stands in for, and the
// nullable-string conversion.

#include "embed/embed.h"
#include "runtime/array.h"
#include "runtime/heap.h"
#include "bronze_host/host_numeric.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// A REAL JS array of `count` items, `make(i)` supplying each: an object with
// numeric keys and a `length` has neither Array.prototype nor an iterator.
// (The DOM's own lists, NodeList and HTMLCollection, are host_dom_lists.h.)
//
// `make(i)` runs with the array already rooted and its result is stored
// immediately, which is what keeps this inside the GC rule: a pre-built
// std::vector<Value> would be stale from its second element onwards.
Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

inline Value makeEmptyArray() {
    return hostArrayOf(0, [](size_t) { return ev::undefined(); });
}

inline Value hostMakeDomError(const char* name, const std::string& message) {
    // Each allocation below may move every value made before it, so each
    // lands in a Persistent, and each string is made in its own statement
    // before the receiver it is stored on is read.
    ev::Persistent msgVal(ev::fromUtf8(message));
    ev::Persistent nameVal(name && *name ? ev::fromUtf8(name) : ev::fromUtf8("Error"));
    auto d = ev::globalValue("DOMException");
    if (d.found && ev::isFunction(d.value)) {
        ev::Persistent ctor(d.value);
        Value args[2] = {msgVal.get(), nameVal.get()};
        ev::CallResult res = ev::construct(ctor.get(), std::span<const Value>(args, 2));
        if (!res.thrown) return res.value;
    }
    ev::Persistent errObj;
    auto g = ev::globalValue("Error");
    if (g.found) {
        Value arg = msgVal.get();
        ev::CallResult res = ev::construct(g.value, std::span<const Value>(&arg, 1));
        errObj.set(res.thrown ? ev::createObject() : res.value);
    } else {
        errObj.set(ev::createObject());
    }
    if (name && *name) {
        errObj.set(ev::setProperty(errObj.get(), "name", nameVal.get()));
    }
    return errObj.get();
}

// A DOMRect, and a 2D DOMMatrix (js/geometry.js, made through the factories
// it puts on `__bro_geometry`; host_geometry_hooks.cpp).
Value makeHostRectValue(double x, double y, double w, double h);
Value makeHostMatrix2DValue(double a, double b, double c, double d, double e, double f);
// A DOMRectList over an array of DOMRects.
Value makeHostRectListValue(Value rects);

// The WebIDL `DOMString?` / [LegacyNullToEmptyString] conversion, which is
// what `textContent`, `innerHTML`, `data` and `nodeValue` are declared as:
// both null and undefined become "".
//
// It has to be spelled out because `ev::isObject(null)` is FALSE in bronze, so
// the `!isObject(v)` guard every setter in this layer uses to reject objects
// lets null straight through to ev::toUtf8 — which stringifies it, and the
// page ends up showing the word "null" where a browser shows nothing. The
// widget spelling that hits it is ordinary:
//     this.label.textContent = value;   // value optional, often omitted
std::string hostNullableString(Value v);

inline Value makeFloat32Array(const float* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeFloat32Array(const std::vector<float>& vec) {
    return makeFloat32Array(vec.data(), vec.size());
}

inline Value makeUint32Array(const uint32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(uint32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeUint32Array(const std::vector<uint32_t>& vec) {
    return makeUint32Array(vec.data(), vec.size());
}

inline Value makeInt32Array(const int32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Int32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(int32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeInt32Array(const std::vector<int32_t>& vec) {
    return makeInt32Array(vec.data(), vec.size());
}

inline Value makeUint8Array(const uint8_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(data, count);
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeUint8Array(const std::vector<uint8_t>& vec) {
    return makeUint8Array(vec.data(), vec.size());
}

inline bool hostIsArray(Value v) {
    return v.isObject() && v.asObject<bronze::HeapObjectHeader>()->flags == bronze::HeapKind::Array;
}

inline bool readFloatVector(Value v, std::vector<float>& out) {
    if (v.isUndefined() || v.isNull()) return false;
    if (!v.isObject()) return false;
    const uint16_t kind = v.asObject<bronze::HeapObjectHeader>()->flags;
    if (kind == bronze::HeapKind::TypedArray) {
        if (auto info = ev::typedArrayInfo(v)) {
            if (info.data && (info.bytesPerElement == sizeof(float) || info.bytesPerElement == 0)) {
                const float* fp = reinterpret_cast<const float*>(info.data);
                out.assign(fp, fp + info.elementCount);
                return true;
            }
            if (info.data && info.bytesPerElement == sizeof(double)) {
                const double* dp = reinterpret_cast<const double*>(info.data);
                out.assign(dp, dp + info.elementCount);
                return true;
            }
        }
    } else if (kind == bronze::HeapKind::Array) {
        const auto* arr = v.asObject<bronze::ArrayHeader>();
        const uint32_t len = arr->length;
        if (len > kMaxHostListLength) return false;
        const Value* elems = arr->elementsData();
        if (len == 0 || (elems && arr->head_offset + len <= arr->capacity)) {
            out.resize(len);
            float* dst = out.data();
            bool allFast = true;
            for (uint32_t i = 0; i < len; ++i) {
                const Value e = elems[i];
                if (e.isNumber()) [[likely]] {
                    dst[i] = static_cast<float>(e.asNumber());
                } else if (e.isHole() || e.isUndefined() || e.isNull()) {
                    dst[i] = 0.0f;
                } else if (e.isBool()) {
                    dst[i] = e.asBool() ? 1.0f : 0.0f;
                } else {
                    allFast = false;
                    break;
                }
            }
            if (allFast) return true;
        }
    }
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (lenV.isUndefined() || lenV.isObject()) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = (!e.isUndefined() && !e.isObject()) ? ev::toDouble(e) : 0.0;
        out.push_back(static_cast<float>(d));
    }
    return true;
}

inline bool readU32Vector(Value v, std::vector<uint32_t>& out) {
    if (v.isUndefined() || v.isNull()) return false;
    if (!v.isObject()) return false;
    const uint16_t kind = v.asObject<bronze::HeapObjectHeader>()->flags;
    if (kind == bronze::HeapKind::TypedArray) {
        if (auto info = ev::typedArrayInfo(v)) {
            if (info.data && (info.bytesPerElement == sizeof(uint32_t) || info.bytesPerElement == 0)) {
                const uint32_t* up = reinterpret_cast<const uint32_t*>(info.data);
                out.assign(up, up + info.elementCount);
                return true;
            }
            if (info.data && info.bytesPerElement == sizeof(uint16_t)) {
                const uint16_t* up = reinterpret_cast<const uint16_t*>(info.data);
                out.clear();
                out.reserve(info.elementCount);
                for (uint32_t i = 0; i < info.elementCount; ++i) out.push_back(up[i]);
                return true;
            }
        }
    } else if (kind == bronze::HeapKind::Array) {
        const auto* arr = v.asObject<bronze::ArrayHeader>();
        const uint32_t len = arr->length;
        if (len > kMaxHostListLength) return false;
        const Value* elems = arr->elementsData();
        if (len == 0 || (elems && arr->head_offset + len <= arr->capacity)) {
            out.resize(len);
            uint32_t* dst = out.data();
            bool allFast = true;
            for (uint32_t i = 0; i < len; ++i) {
                const Value e = elems[i];
                if (e.isNumber()) [[likely]] {
                    dst[i] = satCast<uint32_t>(e.asNumber());
                } else if (e.isHole() || e.isUndefined() || e.isNull()) {
                    dst[i] = 0u;
                } else if (e.isBool()) {
                    dst[i] = e.asBool() ? 1u : 0u;
                } else {
                    allFast = false;
                    break;
                }
            }
            if (allFast) return true;
        }
    }
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (lenV.isUndefined() || lenV.isObject()) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        uint32_t u = (!e.isUndefined() && !e.isObject()) ? satCast<uint32_t>(ev::toDouble(e)) : 0u;
        out.push_back(u);
    }
    return true;
}

}  // namespace bro::bronze_host
