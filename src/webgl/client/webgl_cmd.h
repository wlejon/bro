#pragma once

// The WebGL command records: opcodes, the record header, and the packed
// argument and payload layout both the encoder (webgl_encoder.h) and the
// replay (webgl_replay.cpp) use.
//
// A record is 8-byte aligned and a multiple of 8 bytes long:
//
//   [CmdHeader: u16 op, u16 words][args, packed in table order, unaligned]
//   payload records then add, at payloadDescOffset (4-aligned):
//   [u64 bytes][pad to 8][inline bytes | a blob pointer]
//
// `bytes` is kNullPayload for a call without data (bufferData(target, size,
// usage)); up to kInlinePayloadMax the bytes follow in the record, 8-aligned
// so the backend may read them as the element type; past that the record
// holds a pointer to a malloc'd blob the replay frees.

#include "webgl/client/webgl_cmd_list.h"
#include "webgl/webgl_objects.h"
#include "webgl/webgl_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace bro::webgl {

enum class Op : uint16_t {
    Wrap = 0,  // the rest of the ring's lap is empty: continue at its start
#define BRO_WEBGL_OP(Name, ...) Name,
    BRO_WEBGL_SIMPLE_COMMANDS(BRO_WEBGL_OP)
    BRO_WEBGL_CONTROL_COMMANDS(BRO_WEBGL_OP)
    BRO_WEBGL_PAYLOAD_COMMANDS(BRO_WEBGL_OP, BRO_WEBGL_OP, BRO_WEBGL_OP)
    BRO_WEBGL_BYTES_COMMANDS(BRO_WEBGL_OP)
    BRO_WEBGL_STRING_COMMANDS(BRO_WEBGL_OP)
    BRO_WEBGL_CUSTOM_COMMANDS(BRO_WEBGL_OP)
#undef BRO_WEBGL_OP
    Count,
};

struct CmdHeader {
    uint16_t op;
    uint16_t words;  // the record's length in 8-byte words, header included
};
static_assert(sizeof(CmdHeader) == 4);

/// Payloads up to this size travel inside the record; larger ones in a blob.
inline constexpr size_t kInlinePayloadMax = 16 * 1024;
/// A payload record's `bytes` for "no data".
inline constexpr uint64_t kNullPayload = ~uint64_t{0};

template <class... Ts>
struct TypeList {};

namespace cmd {

constexpr size_t align4(size_t n) { return (n + 3) & ~size_t{3}; }
constexpr size_t align8(size_t n) { return (n + 7) & ~size_t{7}; }

template <class... Ts>
inline constexpr size_t kArgBytes = (size_t{0} + ... + sizeof(Ts));

/// Byte offsets of each argument from the start of the record.
template <class... Ts>
constexpr std::array<size_t, sizeof...(Ts)> argOffsets() {
    std::array<size_t, sizeof...(Ts)> out{};
    size_t at = sizeof(CmdHeader), i = 0;
    ((out[i++] = at, at += sizeof(Ts)), ...);
    return out;
}

/// The record size of a payload-free command.
template <class... Ts>
inline constexpr size_t kSimpleBytes = align8(sizeof(CmdHeader) + kArgBytes<Ts...>);

/// Where a payload record keeps its byte count, and its data.
template <class... Ts>
inline constexpr size_t kPayloadDescOffset = align4(sizeof(CmdHeader) + kArgBytes<Ts...>);
template <class... Ts>
inline constexpr size_t kPayloadDataOffset = align8(kPayloadDescOffset<Ts...> + sizeof(uint64_t));

template <class T>
inline void store(uint8_t* p, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(p, &v, sizeof(T));
}

template <class T>
inline T load(const uint8_t* p) {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

inline void storeHeader(uint8_t* p, Op op, size_t bytes) {
    store(p, CmdHeader{static_cast<uint16_t>(op), static_cast<uint16_t>(bytes / 8)});
}

/// Store `u...` converted to `Ts...` at their packed offsets.
template <class... Ts, class... Us>
inline void storeArgs(uint8_t* rec, TypeList<Ts...>, Us... u) {
    static_assert(sizeof...(Ts) == sizeof...(Us), "argument count differs from the command table");
    [[maybe_unused]] uint8_t* p = rec + sizeof(CmdHeader);
    ((store<Ts>(p, static_cast<Ts>(u)), p += sizeof(Ts)), ...);
}

/// Call f with the record's arguments.
template <class... Ts, class F>
inline void decodeArgs(const uint8_t* rec, F&& f) {
    static constexpr auto offs = argOffsets<Ts...>();
    [&]<size_t... I>(std::index_sequence<I...>) {
        f(load<Ts>(rec + offs[I])...);
    }(std::index_sequence_for<Ts...>{});
    (void)offs;
}

}  // namespace cmd
}  // namespace bro::webgl
