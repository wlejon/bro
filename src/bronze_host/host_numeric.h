#pragma once

// JS number → C++ integer, without undefined behaviour.
//
// A C++ cast of a double that is NaN, infinite, or outside the target type's
// range is undefined behaviour. On x64 it reads back as the "integer
// indefinite" (INT_MIN, 0x80000000, 0x8000000000000000) — or, for the unsigned
// and narrow types, whatever the truncated bit pattern happens to be — and a
// size or an index carries that value on into an allocation, a loop bound or a
// pointer offset. A script chooses every number it passes, so each native that
// turns one into an integer goes through satCast: NaN is 0, a fractional value
// truncates toward zero (the cast's own rule, and WebIDL's for an in-range
// `long`), and anything past the type's range lands on its nearest end.
//
// Saturating makes the conversion defined; it does not make the RESULT a
// sensible size. A native that allocates from a script's number still bounds
// it against what it can actually hold — kMaxHostBufferBytes below for a
// buffer handed back to JS.

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace bro::bronze_host {

template <class T>
inline T satCast(double d) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "satCast converts to an integer type");
    if (std::isnan(d)) return T(0);
    // Both ends as doubles. For a 64-bit type `hi` rounds UP to 2^63 / 2^64,
    // which is exactly the first value the cast cannot take, so `>=` is the
    // right test for every width.
    constexpr double lo = static_cast<double>(std::numeric_limits<T>::min());
    constexpr double hi = static_cast<double>(std::numeric_limits<T>::max());
    if (d <= lo) return std::numeric_limits<T>::min();
    if (d >= hi) return std::numeric_limits<T>::max();
    return static_cast<T>(d);
}

// ECMA-262 ToUint32 / ToInt32 (7.1.7, 7.1.6): NaN and ±Infinity are 0, the
// value truncates toward zero, and the result is taken modulo 2^32. This is
// WebIDL's conversion for `unsigned long` / `long` (GLenum, GLint, a bitmask),
// where a large value wraps rather than saturates.
inline uint32_t jsToUint32(double d) {
    if (!std::isfinite(d)) return 0;
    const double t = std::trunc(d);
    // fmod is exact and lands any finite value in (-2^32, 2^32), so the int64
    // cast is defined; it keeps the sign, which the unsigned conversion then
    // wraps modulo 2^32.
    const double m = std::fmod(t, 4294967296.0);
    const int64_t i = static_cast<int64_t>(m);  // |m| < 2^32: exact, defined
    return static_cast<uint32_t>(i);             // modular, well-defined
}

inline int32_t jsToInt32(double d) {
    return static_cast<int32_t>(jsToUint32(d));  // two's-complement since C++20
}

// A `length` read off an object a script supplied, as a count a native may
// size a vector by and loop over. True with the count when it is a number in
// [0, cap]; false for NaN, a negative value, or one past the cap — the caller
// treats that as a malformed field. A negative length through a plain cast is
// a vector::resize of ~2^64 (std::length_error, which no native boundary
// catches), and a huge one is an allocation the size of the lie.
inline bool lengthWithin(double d, uint32_t cap, uint32_t& out) {
    if (!(d >= 0.0) || d > static_cast<double>(cap)) return false;
    out = static_cast<uint32_t>(d);
    return true;
}

// The cap for an element list a native copies out of a JS array into host
// memory one element at a time: 2^24 entries.
inline constexpr uint32_t kMaxHostListLength = uint32_t{1} << 24;

// The largest buffer a native hands back to JS as one typed array: bronze's
// own per-buffer cap (runtime/typed_array.h kMaxByteLength). A pixel buffer a
// script sizes (createImageData, a readback rectangle) is checked against it
// BEFORE anything is allocated, so an oversized request is the RangeError the
// TypedArray constructor would raise rather than a truncated length or a
// native allocation the size of the request.
inline constexpr uint64_t kMaxHostBufferBytes = uint64_t{1} << 28;

// width*height*4 in 64 bits, and whether that fits one host buffer. Both
// dimensions must already be positive.
inline bool rgbaBufferFits(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return false;
    const uint64_t bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4u;
    return bytes / 4u / static_cast<uint64_t>(w) == static_cast<uint64_t>(h) &&
           bytes < kMaxHostBufferBytes;
}

}  // namespace bro::bronze_host
