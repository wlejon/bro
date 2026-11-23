#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bro::table {

/// Fixed number of rows per columnar chunk (cache-friendly, never resized in place).
inline constexpr uint32_t kChunkRows = 4096;
inline constexpr uint32_t kChunkAliveWords = kChunkRows / 64;
inline constexpr uint32_t kInvalidDictId = 0xFFFFFFFFu;

/// Supported off-heap column element types.
enum class ColType : uint8_t {
    I32 = 0,
    U32,
    I64,
    F32,
    F64,
    Bool,
    Dict,
    String,
};

struct ColumnDef {
    std::string name;
    ColType type = ColType::I32;
};

struct StringSpan {
    uint32_t offset = 0;
    uint32_t len = 0;
};

struct RowLoc {
    uint32_t chunkIdx = 0;
    uint32_t rowIdx = 0;

    bool operator==(const RowLoc& o) const {
        return chunkIdx == o.chunkIdx && rowIdx == o.rowIdx;
    }
};

struct ScalarValue {
    ColType type = ColType::F64;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double f64 = 0.0;
    std::string str;
    bool isNull = false;

    static ScalarValue makeNull() {
        ScalarValue v;
        v.isNull = true;
        return v;
    }
    static ScalarValue makeI32(int32_t x) {
        ScalarValue v;
        v.type = ColType::I32;
        v.i64 = x;
        v.u64 = static_cast<uint64_t>(static_cast<uint32_t>(x));
        v.f64 = static_cast<double>(x);
        return v;
    }
    static ScalarValue makeU32(uint32_t x) {
        ScalarValue v;
        v.type = ColType::U32;
        v.u64 = x;
        v.i64 = static_cast<int64_t>(x);
        v.f64 = static_cast<double>(x);
        return v;
    }
    static ScalarValue makeI64(int64_t x) {
        ScalarValue v;
        v.type = ColType::I64;
        v.i64 = x;
        v.u64 = static_cast<uint64_t>(x);
        v.f64 = static_cast<double>(x);
        return v;
    }
    static ScalarValue makeF32(float x) {
        ScalarValue v;
        v.type = ColType::F32;
        v.f64 = static_cast<double>(x);
        return v;
    }
    static ScalarValue makeF64(double x) {
        ScalarValue v;
        v.type = ColType::F64;
        v.f64 = x;
        return v;
    }
    static ScalarValue makeBool(bool b) {
        ScalarValue v;
        v.type = ColType::Bool;
        v.i64 = b ? 1 : 0;
        v.u64 = b ? 1 : 0;
        v.f64 = b ? 1.0 : 0.0;
        return v;
    }
    static ScalarValue makeDict(uint32_t id, std::string s = {}) {
        ScalarValue v;
        v.type = ColType::Dict;
        v.u64 = id;
        v.i64 = static_cast<int64_t>(id);
        v.f64 = static_cast<double>(id);
        v.str = std::move(s);
        return v;
    }
    static ScalarValue makeString(std::string s) {
        ScalarValue v;
        v.type = ColType::String;
        v.str = std::move(s);
        return v;
    }
};

inline size_t colTypeElemSize(ColType t) {
    switch (t) {
    case ColType::I32: return 4;
    case ColType::U32: return 4;
    case ColType::I64: return 8;
    case ColType::F32: return 4;
    case ColType::F64: return 8;
    case ColType::Bool: return 1;
    case ColType::Dict: return 4;
    case ColType::String: return sizeof(StringSpan);
    }
    return 8;
}

inline bool colTypeIsFloat(ColType t) {
    return t == ColType::F32 || t == ColType::F64;
}

inline bool colTypeIsSignedInt(ColType t) {
    return t == ColType::I32 || t == ColType::I64;
}

inline bool colTypeIsUnsignedInt(ColType t) {
    return t == ColType::U32 || t == ColType::Bool || t == ColType::Dict;
}

inline bool colTypeIsInteger(ColType t) {
    return colTypeIsSignedInt(t) || colTypeIsUnsignedInt(t);
}

inline bool colTypeHasTypedArray(ColType t) {
    return t == ColType::I32 || t == ColType::U32 || t == ColType::I64 ||
           t == ColType::F32 || t == ColType::F64 || t == ColType::Bool ||
           t == ColType::Dict;
}

inline const char* colTypeName(ColType t) {
    switch (t) {
    case ColType::I32: return "i32";
    case ColType::U32: return "u32";
    case ColType::I64: return "i64";
    case ColType::F32: return "f32";
    case ColType::F64: return "f64";
    case ColType::Bool: return "bool";
    case ColType::Dict: return "dict";
    case ColType::String: return "string";
    }
    return "unknown";
}

inline bool parseColType(std::string_view s, ColType& out) {
    if (s == "i32") { out = ColType::I32; return true; }
    if (s == "u32") { out = ColType::U32; return true; }
    if (s == "i64") { out = ColType::I64; return true; }
    if (s == "f32") { out = ColType::F32; return true; }
    if (s == "f64") { out = ColType::F64; return true; }
    if (s == "bool") { out = ColType::Bool; return true; }
    if (s == "dict") { out = ColType::Dict; return true; }
    if (s == "string") { out = ColType::String; return true; }
    return false;
}

/// Canonical 64-bit FNV-1a hash with finalizer for bytes/strings.
inline uint64_t hashBytes(const void* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

/// SplitMix64 mixer for fixed-width 64-bit join/hash/group keys.
inline uint64_t mixHash64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t combineHash64(uint64_t seed, uint64_t value) {
    return seed ^ (mixHash64(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

/// Canonical bit key for numeric / bool / dict values (-0.0 normalized to +0.0).
inline uint64_t canonicalFloatKey(double d) {
    if (d == 0.0) d = 0.0;
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

}  // namespace bro::table
