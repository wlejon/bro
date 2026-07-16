// bro.net.* game-networking bindings (GameNetworkingSockets). Compiled only
// when BRO_WITH_NET is on; the OFF build's NetBindings::install/poll/cleanup
// live in feature_stubs.cpp (install → unavailable bro.net namespace, the rest
// no-ops).
#include "js/net_bindings.h"
#if BRO_WITH_NET

#include "net/net_service.h"
#include "js/message_serializer.h"
#include "js/runtime.h"
#include "util/log.h"

#include "net_sync.js.h"  // generates `static const char js_net_sync[] = ...`

#include <qjsbind/qjsbind.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::js {

// ---------------------------------------------------------------------------
// Wire framing
//
// Every bro.net payload (both directions) starts with a 2-byte header:
//
//   byte 0: kWireMagic (0xB7) — magic + format version in one. If the frame
//           layout ever changes incompatibly, this byte changes, so a peer on
//           the old format drops the message with a loud diagnostic instead
//           of misparsing it.
//   byte 1: frame type — 0x00 raw bytes (send/broadcast), 0x01 structured
//           clone (sendClone/broadcastClone). Unknown types are dropped with
//           a diagnostic, leaving room for future frame kinds.
//
// This is a pre-1.0, bro↔bro wire format change: raw payloads are no longer
// byte-identical on the wire to what send() was given (docs/net-api.js).
// ---------------------------------------------------------------------------
static constexpr uint8_t kWireMagic = 0xB7;
enum WireType : uint8_t {
    kWireRaw   = 0x00,
    kWireClone = 0x01,
};
static constexpr size_t kWireHeaderSize = 2;

// ---------------------------------------------------------------------------
// Per-JSContext state
//
// Stored thread-local because each JSContext lives on exactly one thread
// (engine main thread or one worker thread). Using a static map keyed on
// JSContext* would race on concurrent worker installs — unordered_map
// rehash is not safe across threads.
// ---------------------------------------------------------------------------
struct NetCtxState {
    net::NetService* service = nullptr;
    net::NetSubscriber* subscriber = nullptr;
    JSContext* ctx = nullptr;

    JSValue onConnect = JS_UNDEFINED;
    JSValue onDisconnect = JS_UNDEFINED;
    JSValue onMessage = JS_UNDEFINED;

    bool hosting = false;
    bool hostPending = false;

    // Outgoing connect() calls whose outcome (Connected, or a failure event)
    // has not arrived yet. Keeps hasActivity() true across the handshake so
    // a worker's event loop doesn't block before the result can be polled.
    int connectsPending = 0;

    // Connections we've seen a Connected event for — tracked here since the
    // service thread no longer maintains a per-ctx connection list we can
    // query. Used by bro.net.connections() and bro.net.isHosting().
    std::unordered_map<uint32_t, bool> connections;
};

static thread_local NetCtxState* s_state = nullptr;

static NetCtxState* getState(JSContext* /*ctx*/) {
    return s_state;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string jsToString(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    if (!s) return "";
    std::string result(s);
    JS_FreeCString(ctx, s);
    return result;
}

// Parse the options argument shared by send/broadcast/sendClone/broadcastClone.
// Accepts the legacy boolean (reliable) or an options object
// { reliable?: boolean, channel?: number, nodelay?: boolean }.
// channel is clamped to [0, kNetLaneCount-1] — the lane set is fixed at
// connection setup, so an out-of-range channel silently rides the top lane
// rather than failing the send. Returns false with a JS exception pending on
// an argument that is neither boolean nor object.
static bool parseSendOptions(JSContext* ctx, JSValueConst v, net::SendOptions& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return true;
    if (JS_IsBool(v)) {
        out.reliable = JS_ToBool(ctx, v);
        return true;
    }
    if (JS_IsArray(v)) {
        // Someone reaching for postMessage's (value, transferList) shape.
        JS_ThrowTypeError(ctx, "bro.net: transfer lists are not supported over "
                               "the network; pass an options object");
        return false;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "bro.net: options must be a boolean (reliable) or "
                               "{reliable, channel, nodelay}");
        return false;
    }
    JSValue r = JS_GetPropertyStr(ctx, v, "reliable");
    if (!JS_IsUndefined(r)) out.reliable = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);

    JSValue c = JS_GetPropertyStr(ctx, v, "channel");
    if (!JS_IsUndefined(c)) {
        int32_t ch = 0;
        JS_ToInt32(ctx, &ch, c);
        if (ch < 0) ch = 0;
        if (ch >= net::kNetLaneCount) ch = net::kNetLaneCount - 1;
        out.channel = ch;
    }
    JS_FreeValue(ctx, c);

    JSValue n = JS_GetPropertyStr(ctx, v, "nodelay");
    if (!JS_IsUndefined(n)) out.nodelay = JS_ToBool(ctx, n);
    JS_FreeValue(ctx, n);
    return true;
}

// Append the bytes of a string / ArrayBuffer / TypedArray payload to `out`.
// Returns false with a JS exception pending for any other type.
static bool appendPayloadBytes(JSContext* ctx, JSValueConst data, std::vector<uint8_t>& out) {
    if (JS_IsString(data)) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, data);
        if (!s) return false;
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(s),
                   reinterpret_cast<const uint8_t*>(s) + len);
        JS_FreeCString(ctx, s);
        return true;
    }
    size_t size = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &size, data);
    if (!buf) {
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, data, &offset, &blen, nullptr);
        if (JS_IsException(abuf)) {
            JS_ThrowTypeError(ctx, "bro.net: data must be a string, ArrayBuffer, "
                                   "or TypedArray");
            return false;
        }
        buf = JS_GetArrayBuffer(ctx, &size, abuf);
        JS_FreeValue(ctx, abuf);
        if (!buf) return false;  // detached — exception already pending
        buf += offset;
        size = blen;
    }
    if (size > 0) out.insert(out.end(), buf, buf + size);
    return true;
}

// ---------------------------------------------------------------------------
// bro.net.init() → boolean
//
// Retained for backwards compatibility with existing apps; the service is
// already initialized before bindings are installed, so this is a no-op
// success.
// ---------------------------------------------------------------------------
static JSValue js_net_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, getState(ctx) != nullptr);
}

static JSValue js_net_host(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s) return JS_ThrowInternalError(ctx, "bro.net not initialized");
    if (argc < 1) return JS_ThrowTypeError(ctx, "host() requires a port number");

    int32_t port = 0;
    JS_ToInt32(ctx, &port, argv[0]);
    if (port < 1 || port > 65535) return JS_ThrowRangeError(ctx, "port must be 1..65535");

    s->hostPending = true;
    s->subscriber->host(static_cast<uint16_t>(port));
    return JS_TRUE;
}

static JSValue js_net_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s) return JS_ThrowInternalError(ctx, "bro.net not initialized");
    if (argc < 1) return JS_ThrowTypeError(ctx, "connect() requires an address string");

    std::string addr = jsToString(ctx, argv[0]);
    s->connectsPending++;
    s->subscriber->connect(addr);
    return JS_TRUE;
}

static JSValue js_net_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 2) return JS_FALSE;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    net::SendOptions opts;
    if (argc >= 3 && !parseSendOptions(ctx, argv[2], opts)) return JS_EXCEPTION;

    std::vector<uint8_t> framed;
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    if (!appendPayloadBytes(ctx, argv[1], framed)) return JS_EXCEPTION;

    s->subscriber->send(conn, std::move(framed), opts);
    return JS_TRUE;
}

static JSValue js_net_broadcast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 1) return JS_UNDEFINED;

    net::SendOptions opts;
    if (argc >= 2 && !parseSendOptions(ctx, argv[1], opts)) return JS_EXCEPTION;

    std::vector<uint8_t> framed;
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    if (!appendPayloadBytes(ctx, argv[0], framed)) return JS_EXCEPTION;

    s->subscriber->broadcast(std::move(framed), opts);
    return JS_UNDEFINED;
}

// Serialize `value` as a network structured clone and frame it. Shared by
// sendClone/broadcastClone. Returns false with a JS exception pending on
// non-clonable values (functions, Mesh, ImageBitmap, ...).
static bool buildCloneFrame(JSContext* ctx, JSValueConst value, std::vector<uint8_t>& framed) {
    Message msg;
    if (!serializeMessage(ctx, value, JS_UNDEFINED, msg, /*forNetwork=*/true))
        return false;
    // forNetwork serialization can never emit pointer-transfer slots; if it
    // somehow did, sending would leak dangling pointers to the peer. Guard it.
    if (!msg.transferredBuffers.empty() || !msg.transferredObjects.empty()) {
        JS_ThrowTypeError(ctx, "sendClone: value contains objects that cannot "
                               "cross a network");
        return false;
    }
    framed.reserve(kWireHeaderSize + msg.data.size());
    framed.push_back(kWireMagic);
    framed.push_back(kWireClone);
    framed.insert(framed.end(), msg.data.begin(), msg.data.end());
    return true;
}

static JSValue js_net_sendClone(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s) return JS_ThrowInternalError(ctx, "bro.net not initialized");
    if (argc < 2) return JS_ThrowTypeError(ctx, "sendClone() requires (connId, value)");

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    net::SendOptions opts;
    if (argc >= 3 && !parseSendOptions(ctx, argv[2], opts)) return JS_EXCEPTION;

    std::vector<uint8_t> framed;
    if (!buildCloneFrame(ctx, argv[1], framed)) return JS_EXCEPTION;

    s->subscriber->send(conn, std::move(framed), opts);
    return JS_TRUE;
}

static JSValue js_net_broadcastClone(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s) return JS_ThrowInternalError(ctx, "bro.net not initialized");
    if (argc < 1) return JS_ThrowTypeError(ctx, "broadcastClone() requires a value");

    net::SendOptions opts;
    if (argc >= 2 && !parseSendOptions(ctx, argv[1], opts)) return JS_EXCEPTION;

    std::vector<uint8_t> framed;
    if (!buildCloneFrame(ctx, argv[0], framed)) return JS_EXCEPTION;

    s->subscriber->broadcast(std::move(framed), opts);
    return JS_UNDEFINED;
}

// Test hook (undocumented): send bytes verbatim, with NO wire header. This is
// what a buggy or hostile peer looks like to the receiver, so tests use it to
// prove the receive path drops unrecognized/malformed frames instead of
// crashing. Not part of the public API.
static JSValue js_net_sendUnframed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 2) return JS_FALSE;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    net::SendOptions opts;
    if (argc >= 3 && !parseSendOptions(ctx, argv[2], opts)) return JS_EXCEPTION;

    std::vector<uint8_t> raw;
    if (!appendPayloadBytes(ctx, argv[1], raw)) return JS_EXCEPTION;

    s->subscriber->send(conn, std::move(raw), opts);
    return JS_TRUE;
}

static JSValue js_net_disconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 1) return JS_UNDEFINED;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);
    int reason = 0;
    if (argc >= 2) JS_ToInt32(ctx, &reason, argv[1]);

    s->subscriber->disconnect(conn, reason);
    s->connections.erase(conn);
    return JS_UNDEFINED;
}

static JSValue js_net_close(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState(ctx);
    if (!s) return JS_UNDEFINED;
    s->subscriber->closeHost();
    s->hosting = false;
    s->connections.clear();
    return JS_UNDEFINED;
}

static JSValue js_net_stats(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 1) return JS_NULL;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    net::ConnectionStats stats;
    if (!s->subscriber->getConnectionStats(conn, stats)) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ping", JS_NewFloat64(ctx, stats.ping));
    JS_SetPropertyStr(ctx, obj, "packetLoss", JS_NewFloat64(ctx, stats.packetLoss));
    JS_SetPropertyStr(ctx, obj, "bytesSent", JS_NewFloat64(ctx, stats.bytesPerSecSent));
    JS_SetPropertyStr(ctx, obj, "bytesRecv", JS_NewFloat64(ctx, stats.bytesPerSecRecv));
    return obj;
}

static JSValue js_net_connections(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!s) return arr;
    uint32_t i = 0;
    for (auto& [conn, _] : s->connections) {
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewUint32(ctx, conn));
    }
    return arr;
}

static JSValue js_net_isHosting(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState(ctx);
    return JS_NewBool(ctx, s && s->hosting);
}

// ---------------------------------------------------------------------------
// Callback property accessors — straightforward get/set of stored JSValues.
// ---------------------------------------------------------------------------
#define CB_ACCESSORS(name, field)                                              \
    static JSValue js_net_get_##name(JSContext* ctx, JSValueConst, int, JSValueConst*) { \
        auto* s = getState(ctx);                                               \
        return s ? JS_DupValue(ctx, s->field) : JS_UNDEFINED;                  \
    }                                                                          \
    static JSValue js_net_set_##name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { \
        auto* s = getState(ctx);                                               \
        if (!s) return JS_UNDEFINED;                                           \
        JS_FreeValue(ctx, s->field);                                           \
        s->field = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;      \
        return JS_UNDEFINED;                                                   \
    }

CB_ACCESSORS(onconnect, onConnect)
CB_ACCESSORS(ondisconnect, onDisconnect)
CB_ACCESSORS(onmessage, onMessage)
#undef CB_ACCESSORS

// ---------------------------------------------------------------------------
// Function list for bro.net namespace
// ---------------------------------------------------------------------------
static const JSCFunctionListEntry js_net_funcs[] = {
    JS_CFUNC_DEF("init", 0, js_net_init),
    JS_CFUNC_DEF("host", 1, js_net_host),
    JS_CFUNC_DEF("connect", 1, js_net_connect),
    JS_CFUNC_DEF("send", 3, js_net_send),
    JS_CFUNC_DEF("broadcast", 2, js_net_broadcast),
    JS_CFUNC_DEF("sendClone", 3, js_net_sendClone),
    JS_CFUNC_DEF("broadcastClone", 2, js_net_broadcastClone),
    JS_CFUNC_DEF("_sendUnframed", 3, js_net_sendUnframed),
    JS_CFUNC_DEF("disconnect", 2, js_net_disconnect),
    JS_CFUNC_DEF("close", 0, js_net_close),
    JS_CFUNC_DEF("stats", 1, js_net_stats),
    JS_CFUNC_DEF("connections", 0, js_net_connections),
    JS_CFUNC_DEF("isHosting", 0, js_net_isHosting),
};

// ---------------------------------------------------------------------------
// Install / Cleanup / Poll
// ---------------------------------------------------------------------------
void NetBindings::install(JSContext* ctx, net::NetService* service) {
    if (s_state) {
        LOG_WARN("[net] NetBindings::install called twice on the same thread");
        return;
    }
    auto* state = new NetCtxState();
    state->service = service;
    state->ctx = ctx;
    state->subscriber = service->createSubscriber();
    s_state = state;

    // Wire subscriber callbacks → JS callbacks on this context.
    // These fire synchronously during poll() on this context's thread.
    state->subscriber->onHostResult = [ctx](bool success) {
        auto* s = getState(ctx);
        if (!s) return;
        s->hostPending = false;
        s->hosting = success;
    };
    state->subscriber->onConnectResult = [ctx](bool success) {
        // Initiation ack only — app listens for onConnect for the actual
        // link. A failed initiation is the end of that connect attempt.
        auto* s = getState(ctx);
        if (s && !success && s->connectsPending > 0) s->connectsPending--;
    };
    state->subscriber->onConnect = [ctx](uint32_t conn) {
        auto* s = getState(ctx);
        if (!s) return;
        if (s->connectsPending > 0) s->connectsPending--;
        s->connections[conn] = true;
        if (!JS_IsUndefined(s->onConnect) && !JS_IsNull(s->onConnect)) {
            JSValue func = JS_DupValue(ctx, s->onConnect);
            JSValue arg = JS_NewUint32(ctx, conn);
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(ret)) Runtime::checkException(ctx, ret);
            else JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        }
    };
    state->subscriber->onDisconnect = [ctx](uint32_t conn, int reason) {
        auto* s = getState(ctx);
        if (!s) return;
        if (s->connections.erase(conn) == 0 && s->connectsPending > 0) {
            // A conn we never saw Connected — an outgoing connect that
            // failed after initiation.
            s->connectsPending--;
        }
        if (!JS_IsUndefined(s->onDisconnect) && !JS_IsNull(s->onDisconnect)) {
            JSValue func = JS_DupValue(ctx, s->onDisconnect);
            JSValue args[2] = {
                JS_NewUint32(ctx, conn),
                JS_NewInt32(ctx, reason),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, args);
            if (JS_IsException(ret)) Runtime::checkException(ctx, ret);
            else JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, func);
        }
    };
    state->subscriber->onMessage = [ctx](net::NetworkMessage&& msg) {
        auto* s = getState(ctx);
        if (!s) return;
        if (JS_IsUndefined(s->onMessage) || JS_IsNull(s->onMessage)) return;

        // Parse the wire frame. The peer is untrusted: anything that doesn't
        // carry our magic + a known frame type is dropped with a diagnostic —
        // never surfaced to JS, never allowed to crash.
        if (msg.data.size() < kWireHeaderSize || msg.data[0] != kWireMagic) {
            LOG_WARN("[net] conn %u: dropping message with unrecognized wire "
                     "framing (%zu bytes)", msg.connection, msg.data.size());
            return;
        }
        const uint8_t frameType = msg.data[1];

        JSValue payload;
        if (frameType == kWireRaw) {
            payload = JS_NewArrayBufferCopy(ctx, msg.data.data() + kWireHeaderSize,
                                            msg.data.size() - kWireHeaderSize);
        } else if (frameType == kWireClone) {
            // Deserialize in THIS context — each subscriber's onMessage runs
            // on its own JS context's thread (main or worker), so values are
            // materialized where they will be used.
            Message m;
            m.data = std::move(msg.data);
            payload = deserializeMessage(ctx, m, kWireHeaderSize);
            if (JS_IsException(payload)) {
                // Malformed clone payload (truncated, hostile, or version
                // skew). Drop with a diagnostic; the connection stays up.
                JSValue exc = JS_GetException(ctx);
                const char* what = JS_ToCString(ctx, exc);
                LOG_WARN("[net] conn %u: dropping malformed clone payload: %s",
                         msg.connection, what ? what : "(unknown)");
                if (what) JS_FreeCString(ctx, what);
                JS_FreeValue(ctx, exc);
                return;
            }
        } else {
            LOG_WARN("[net] conn %u: dropping message with unknown frame type "
                     "0x%02x", msg.connection, frameType);
            return;
        }

        JSValue func = JS_DupValue(ctx, s->onMessage);
        JSValue args[3] = {
            JS_NewUint32(ctx, msg.connection),
            payload,
            JS_NewInt32(ctx, msg.channel),
        };
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 3, args);
        if (JS_IsException(ret)) Runtime::checkException(ctx, ret);
        else JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, func);
    };

    // Build bro.net namespace.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue netObj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, netObj, js_net_funcs,
                               sizeof(js_net_funcs) / sizeof(js_net_funcs[0]));

    JSAtom aConnect = JS_NewAtom(ctx, "onconnect");
    JS_DefinePropertyGetSet(ctx, netObj, aConnect,
        JS_NewCFunction(ctx, js_net_get_onconnect, "get onconnect", 0),
        JS_NewCFunction(ctx, js_net_set_onconnect, "set onconnect", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aConnect);

    JSAtom aDisconnect = JS_NewAtom(ctx, "ondisconnect");
    JS_DefinePropertyGetSet(ctx, netObj, aDisconnect,
        JS_NewCFunction(ctx, js_net_get_ondisconnect, "get ondisconnect", 0),
        JS_NewCFunction(ctx, js_net_set_ondisconnect, "set ondisconnect", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aDisconnect);

    JSAtom aMessage = JS_NewAtom(ctx, "onmessage");
    JS_DefinePropertyGetSet(ctx, netObj, aMessage,
        JS_NewCFunction(ctx, js_net_get_onmessage, "get onmessage", 0),
        JS_NewCFunction(ctx, js_net_set_onmessage, "set onmessage", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aMessage);

    JS_SetPropertyStr(ctx, broObj, "net", netObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);

    // --- bro.net.sync — Godot-style high-level multiplayer (spawn/despawn
    //     replication, authority, delta state sync, RPC) layered in pure JS
    //     over the primitives just installed. Runs in every context with a
    //     real bro.net (main + workers); the BRO_WITH_NET=OFF stub install
    //     never evaluates it, so bro.net.sync simply doesn't exist there. ---
    JSValue r = JS_Eval(ctx, js_net_sync, std::strlen(js_net_sync),
                        "<bro.net.sync>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char* what = JS_ToCString(ctx, exc);
        LOG_ERROR("[net] bro.net.sync install failed: %s", what ? what : "(unknown)");
        if (what) JS_FreeCString(ctx, what);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}

void NetBindings::cleanup(JSContext* ctx) {
    NetCtxState* s = s_state;
    if (!s) return;

    // Null out thread-local first so any late callback (however unlikely —
    // subscriber->poll() is single-threaded with us) becomes a no-op.
    s_state = nullptr;

    if (ctx) {
        JS_FreeValue(ctx, s->onConnect);
        JS_FreeValue(ctx, s->onDisconnect);
        JS_FreeValue(ctx, s->onMessage);
    }

    // Detach the subscriber. The service thread will close any sockets it
    // owns and free the subscriber asynchronously.
    if (s->service && s->subscriber) {
        s->service->destroySubscriber(s->subscriber);
    }

    delete s;
}

void NetBindings::poll(JSContext* ctx) {
    auto* s = getState(ctx);
    if (!s || !s->subscriber) return;
    s->subscriber->poll();
}

bool NetBindings::hasActivity(JSContext* ctx) {
    auto* s = getState(ctx);
    if (!s || !s->subscriber) return false;
    return s->hosting || s->hostPending || s->connectsPending > 0 ||
           !s->connections.empty() || s->subscriber->hasQueuedEvents();
}

} // namespace bro::js

#endif  // BRO_WITH_NET
