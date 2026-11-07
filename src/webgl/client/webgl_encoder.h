#pragma once

// Encoding one record into a WebGLRing: a reserve (one compare against the
// cached limit), the header and argument stores at constant offsets, and the
// payload copy. No atomics: publishing is the client's, at its sync points.

#include "webgl/client/webgl_blob.h"
#include "webgl/client/webgl_cmd.h"
#include "webgl/client/webgl_ring.h"

namespace bro::webgl {

/// A payload-free record.
template <class... Ts, class... Us>
[[gnu::always_inline]] inline void encodeSimple(WebGLRing& ring, Op op, TypeList<Ts...> types, Us... u) {
    constexpr size_t bytes = cmd::kSimpleBytes<Ts...>;
    uint8_t* rec = ring.reserve(bytes);
    cmd::storeHeader(rec, op, bytes);
    cmd::storeArgs(rec, types, u...);
}

/// A record with a payload: `bytes` of `data` (kNullPayload: none), inline up
/// to kInlinePayloadMax, else `blob` (the caller's copy, already made, which
/// the replay gives back to the pool).
template <class... Ts, class... Us>
[[gnu::always_inline]] inline void encodePayload(WebGLRing& ring, Op op, TypeList<Ts...> types, const void* data,
                                                 uint64_t bytes, WebGLBlob* blob, Us... u) {
    constexpr size_t descAt = cmd::kPayloadDescOffset<Ts...>;
    constexpr size_t dataAt = cmd::kPayloadDataOffset<Ts...>;
    size_t body = 0;
    if (bytes != kNullPayload) body = bytes <= kInlinePayloadMax ? static_cast<size_t>(bytes) : sizeof(void*);
    const size_t total = cmd::align8(dataAt + body);
    uint8_t* rec = ring.reserve(total);
    cmd::storeHeader(rec, op, total);
    cmd::storeArgs(rec, types, u...);
    cmd::store<uint64_t>(rec + descAt, bytes);
    if (bytes == kNullPayload) return;
    if (bytes <= kInlinePayloadMax)
        std::memcpy(rec + dataAt, data, static_cast<size_t>(bytes));
    else
        cmd::store<WebGLBlob*>(rec + dataAt, blob);
}

/// The payload of a record encodePayload wrote: its data (null for none) and
/// size, and the blob holding the data, if any (the reader gives it back).
struct PayloadView {
    const void* data = nullptr;
    uint64_t bytes = 0;
    WebGLBlob* blob = nullptr;
};

template <class... Ts>
inline PayloadView payloadOf(const uint8_t* rec, TypeList<Ts...>) {
    PayloadView v;
    v.bytes = cmd::load<uint64_t>(rec + cmd::kPayloadDescOffset<Ts...>);
    if (v.bytes == kNullPayload) {
        v.bytes = 0;
        return v;
    }
    const uint8_t* at = rec + cmd::kPayloadDataOffset<Ts...>;
    if (v.bytes <= kInlinePayloadMax) {
        v.data = at;
    } else {
        v.blob = cmd::load<WebGLBlob*>(at);
        v.data = v.blob->bytes.data();
    }
    return v;
}

}  // namespace bro::webgl
