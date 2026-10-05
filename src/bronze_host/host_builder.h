#pragma once

// The plumbing every bronze_host binding shares: defensive argument readers,
// bulk-data readers over typed arrays and plain arrays, and ObjectBuilder,
// which builds a host object property by property in a fixed order.
//
// GC DISCIPLINE (the one rule of this layer): bronze's heap is a moving
// semispace collector. A Value held in a plain C++ variable is stale after
// the next allocating embed call; anything held across one lives in a
// bronze::embed::Persistent. Pointers from embed::typedArrayInfo() die at the
// next bronze allocation — every binding that takes one hands it to the call
// that consumes it (which copies) before any embed call that could allocate,
// and never stores it.

#include "bronze_host/host_numeric.h"
#include "bronze_host/host_profile.h"
#include "bronze_host/host_rooted.h"

#include "embed/embed.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// Argument readers
// ---------------------------------------------------------------------------

// Defensive by design: embed::toDouble on an OBJECT is a hard runtime error
// (rt_convert.cpp), and a padded missing argument arrives as undefined (NaN).
// Argument decoding must never take the process down over a bad call, so
// objects and NaN read as 0.
inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

// WebIDL `long` / `unsigned long`: ToInt32 / ToUint32, so a value past the
// type's range wraps (0x8xxxxxxx bitmasks stay exact) and ±Infinity or 1e300
// is 0 — a plain cast of either is undefined behaviour.
inline int32_t i32At(std::span<const Value> args, size_t i) {
    return jsToInt32(numAt(args, i));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return jsToUint32(numAt(args, i));
}

inline int64_t i64At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return ev::toInt64(args[i]);
}

inline uint64_t u64At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return ev::toUint64(args[i]);
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return ev::toBool(args[i]);
}

inline Value argAt(std::span<const Value> args, size_t i) {
    return i < args.size() ? args[i] : ev::undefined();
}

// Was argument `i` actually supplied?
//
// NOT the same question as `i < args.size()`. Compiled code calling a host
// function through the direct-dispatch path passes one value per DECLARED
// parameter, padding the ones the call site left off with `undefined` — so a
// two-argument JS call into a def() of arity 8 arrives here with args.size()
// == 8. Deciding optionality on the span's length therefore reads a padded
// `undefined` as if the app had passed it: numAt() answers 0, and an optional
// layer mask silently becomes "match nothing". Ask about the VALUE instead.
inline bool hasArg(std::span<const Value> args, size_t i) {
    return i < args.size() && !ev::isUndefined(args[i]);
}

// ---------------------------------------------------------------------------
// Bulk-data readers (uniform*v, bufferData, texImage2D, ...)
// ---------------------------------------------------------------------------

// A typed array answers its heap bytes directly — VALID ONLY UNTIL THE NEXT
// BRONZE ALLOCATION, so the call consuming it must be the very next thing that
// happens. A plain JS array (three.js hands those to uniform*fv and
// drawBuffers) is copied element by element into `storage` via embed reads;
// the copy is host memory and stable. False when the value is neither.
// Shared plain-array walk behind the three element flavours below. The
// receiver rides in a Persistent for the whole loop because every
// getProperty/getElement read ALLOCATES (the key string) and would leave a
// raw Value copy pointing into dead from-space — the exact stale-pointer bug
// the GC contract at the top of this header exists to prevent.
template <typename T, typename Convert>
inline bool plainArrayData(Value v, std::vector<T>& storage, Convert convert,
                           const T** outData, size_t* outCount) {
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    // `length` is whatever the object says: bounded like any buffer a script
    // sizes, so `{length: 4e9}` is refused rather than a 16 GB resize.
    uint32_t n = satCast<uint32_t>(ev::toDouble(lenV));
    if (static_cast<uint64_t>(n) * sizeof(T) >= kMaxHostBufferBytes) return false;
    storage.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(root.get(), i);
        // NaN (a hole, an undefined pad) converts as 0 for the integer
        // flavours — casting NaN to an integer is UB, not 0.
        double d = ev::isObject(e) ? 0.0 : ev::toDouble(e);
        storage[i] = std::isnan(d) ? T{} : convert(d);
    }
    *outData = storage.data();
    *outCount = n;
    return true;
}

inline bool floatData(Value v, std::vector<float>& storage,
                      const float** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const float*>(info.data);
        *outCount = info.byteLength / sizeof(float);
        return true;
    }
    return plainArrayData<float>(
        v, storage, [](double d) { return static_cast<float>(d); }, outData, outCount);
}

inline bool int32Data(Value v, std::vector<int32_t>& storage,
                      const int32_t** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const int32_t*>(info.data);
        *outCount = info.byteLength / sizeof(int32_t);
        return true;
    }
    return plainArrayData<int32_t>(
        v, storage,
        [](double d) { return jsToInt32(d); },
        outData, outCount);
}

inline bool uint32Data(Value v, std::vector<uint32_t>& storage,
                       const uint32_t** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const uint32_t*>(info.data);
        *outCount = info.byteLength / sizeof(uint32_t);
        return true;
    }
    return plainArrayData<uint32_t>(
        v, storage,
        [](double d) { return jsToUint32(d); },
        outData, outCount);
}

// Raw bytes of a typed array OR an ArrayBuffer, with the element size the
// WebGL2 srcOffset/length overloads count in (1 for a bare buffer) — the
// bronze twin of getBufferDataEx(). The pointer is heap-borrowed: consume it
// in the very next call, allocate nothing in between.
inline bool bufferBytes(Value v, const uint8_t** outData, size_t* outLen,
                        size_t* outElemSize) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = info.data;
        *outLen = info.byteLength;
        *outElemSize = info.bytesPerElement ? info.bytesPerElement : 1;
        return true;
    }
    if (auto buf = ev::arrayBufferInfo(v)) {
        *outData = buf.data;
        *outLen = buf.byteLength;
        *outElemSize = 1;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Deterministic property registration
// ---------------------------------------------------------------------------

// Builds the context object property by property. The Persistent is the GC
// anchor: setProperty allocates and may move the object, so every def() both
// re-reads the current address and stores the post-call one. Registration
// order is exactly source order — a fixed sequence of def() calls, never an
// iteration over an unordered container — which is what keeps the object's
// shape (and bronze's inline caches over it) deterministic run to run.
struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(const char* name, Value v) {
        // `v` is not held across any allocation here: setProperty roots its
        // arguments internally, and obj.get() does not allocate.
        obj.set(ev::setProperty(obj.get(), name, v));
    }

    void def(const char* name, uint32_t arity, ev::NativeFn fn) {
        // hostProfileWrap is the identity unless BRO_HOST_PROFILE=1 (host_profile.h);
        // makeFunction allocates, so it runs BEFORE obj.get() is read.
        //
        // The name is passed on, and it is a fact rather than a courtesy: a
        // host method standing in for a web-platform one answers for that
        // method's `.name` too (`gl.drawElements.name === "drawElements"` in
        // every browser), and a profile of the runtime cannot otherwise tell
        // 166 WebGL entry points apart — every host function shares ONE
        // trampoline code pointer, so the name slot is the only thing that
        // distinguishes them. One line here names every method reached through
        // this builder, DOM included, for the same reason hostProfileWrap's
        // comment gives: they all funnel through one place.
        ev::Persistent f(ev::makeFunction(hostProfileWrap(name, std::move(fn)), arity, name));
        obj.set(ev::setProperty(obj.get(), name, f.get()));
    }

    void accessor(const char* name, ev::NativeFn getter, ev::NativeFn setter) {
        // The getter must survive the setter's allocation, hence the
        // Persistent bridge between the two makeFunction calls.
        //
        // "get x" / "set x", which is 10.2.9's own spelling for an accessor
        // function's name (SetFunctionName with a prefix), and not the bare
        // property name the two would otherwise share.
        const std::string getName = "get " + std::string(name);
        const std::string setName = "set " + std::string(name);
        // The setter is rooted too: nothing between its makeFunction and
        // defineAccessor may be assumed allocation-free.
        //
        // The names are the functions' own (FunctionHeader::name), which is
        // also what a bronze profile report names a host callee by.
        ev::Persistent g(
            ev::makeFunction(hostProfileWrap(name, std::move(getter)), 0, getName));
        ev::Persistent s(setter ? ev::makeFunction(hostProfileWrap(name, std::move(setter)), 1,
                                                   setName)
                                : ev::undefined());
        obj.set(ev::defineAccessor(obj.get(), name, g.get(), s.get(), /*enumerable=*/true));
    }

    Value get() const { return obj.get(); }
};
// A real JS Array of `count` elements, element i from make(i).
Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

class HostClass;

}  // namespace bro::bronze_host
