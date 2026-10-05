#pragma once

// Building and reading plain values: real arrays, typed arrays in and out,
// DOMRect-shaped objects, the Error a DOMException stands in for, and the
// nullable-string conversion.

#include "embed/embed.h"
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

// A REAL JS array of `count` items, `make(i)` supplying each. Real, because
// what an app does with `children` or `querySelectorAll` is iterate it —
// `for…of`, `Array.from`, `.map` — and an object with numeric keys and a
// `length` has neither Array.prototype nor an iterator, so every one of those
// is a TypeError at the call site rather than an empty result.
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
        ev::Persistent nameVal(ev::fromUtf8(name));
        errObj.set(ev::setProperty(errObj.get(), "name", nameVal.get()));
    }
    return errObj.get();
}

// A DOMRect-shaped plain object — x/y/width/height plus the four edges.
Value makeHostRectValue(double x, double y, double w, double h);

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
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
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
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d =(!ev::isUndefined(e) && !ev::isObject(e)) ? ev::toDouble(e) : 0.0;
        out.push_back(static_cast<float>(d));
    }
    return true;
}

inline bool readU32Vector(Value v, std::vector<uint32_t>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
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
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = 0;
    if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, len)) return false;
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        uint32_t u =(!ev::isUndefined(e) && !ev::isObject(e)) ? satCast<uint32_t>(ev::toDouble(e)) : 0u;
        out.push_back(u);
    }
    return true;
}

}  // namespace bro::bronze_host
