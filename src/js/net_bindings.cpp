#include "js/net_bindings.h"
#include "net/net_service.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace bro::js {

// ---------------------------------------------------------------------------
// Per-JSContext state
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

    // Connections we've seen a Connected event for — tracked here since the
    // service thread no longer maintains a per-ctx connection list we can
    // query. Used by bro.net.connections() and bro.net.isHosting().
    std::unordered_map<uint32_t, bool> connections;
};

static std::unordered_map<JSContext*, NetCtxState> s_states;

static NetCtxState* getState(JSContext* ctx) {
    auto it = s_states.find(ctx);
    return (it == s_states.end()) ? nullptr : &it->second;
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
    s->subscriber->connect(addr);
    return JS_TRUE;
}

static JSValue js_net_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 2) return JS_FALSE;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    bool reliable = true;
    if (argc >= 3) reliable = JS_ToBool(ctx, argv[2]);

    if (JS_IsString(argv[1])) {
        std::string str = jsToString(ctx, argv[1]);
        s->subscriber->send(conn, str.data(), static_cast<uint32_t>(str.size()), reliable);
        return JS_TRUE;
    }

    size_t size = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf) {
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &blen, nullptr);
        if (!JS_IsException(abuf)) {
            buf = JS_GetArrayBuffer(ctx, &size, abuf);
            JS_FreeValue(ctx, abuf);
            if (buf) { buf += offset; size = blen; }
        } else {
            JS_FreeValue(ctx, abuf);
            return JS_ThrowTypeError(ctx, "send() data must be a string, ArrayBuffer, or TypedArray");
        }
    }
    if (!buf) return JS_FALSE;
    s->subscriber->send(conn, buf, static_cast<uint32_t>(size), reliable);
    return JS_TRUE;
}

static JSValue js_net_broadcast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState(ctx);
    if (!s || argc < 1) return JS_UNDEFINED;

    bool reliable = true;
    if (argc >= 2) reliable = JS_ToBool(ctx, argv[1]);

    if (JS_IsString(argv[0])) {
        std::string str = jsToString(ctx, argv[0]);
        s->subscriber->broadcast(str.data(), static_cast<uint32_t>(str.size()), reliable);
        return JS_UNDEFINED;
    }
    size_t size = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!buf) {
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &blen, nullptr);
        if (!JS_IsException(abuf)) {
            buf = JS_GetArrayBuffer(ctx, &size, abuf);
            JS_FreeValue(ctx, abuf);
            if (buf) { buf += offset; size = blen; }
        } else {
            JS_FreeValue(ctx, abuf);
        }
    }
    if (buf) s->subscriber->broadcast(buf, static_cast<uint32_t>(size), reliable);
    return JS_UNDEFINED;
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
    NetCtxState state;
    state.service = service;
    state.ctx = ctx;
    state.subscriber = service->createSubscriber();

    // Wire subscriber callbacks → JS callbacks on this context.
    // These fire synchronously during poll() on this context's thread.
    state.subscriber->onHostResult = [ctx](bool success) {
        auto* s = getState(ctx);
        if (!s) return;
        s->hostPending = false;
        s->hosting = success;
    };
    state.subscriber->onConnectResult = [](bool) {
        // Initiation ack only — app listens for onConnect for the actual link.
    };
    state.subscriber->onConnect = [ctx](uint32_t conn) {
        auto* s = getState(ctx);
        if (!s) return;
        s->connections[conn] = true;
        if (!JS_IsUndefined(s->onConnect) && !JS_IsNull(s->onConnect)) {
            JSValue func = JS_DupValue(ctx, s->onConnect);
            JSValue arg = JS_NewUint32(ctx, conn);
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        }
    };
    state.subscriber->onDisconnect = [ctx](uint32_t conn, int reason) {
        auto* s = getState(ctx);
        if (!s) return;
        s->connections.erase(conn);
        if (!JS_IsUndefined(s->onDisconnect) && !JS_IsNull(s->onDisconnect)) {
            JSValue func = JS_DupValue(ctx, s->onDisconnect);
            JSValue args[2] = {
                JS_NewUint32(ctx, conn),
                JS_NewInt32(ctx, reason),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, func);
        }
    };
    state.subscriber->onMessage = [ctx](net::NetworkMessage&& msg) {
        auto* s = getState(ctx);
        if (!s) return;
        if (JS_IsUndefined(s->onMessage) || JS_IsNull(s->onMessage)) return;
        JSValue func = JS_DupValue(ctx, s->onMessage);
        JSValue ab = JS_NewArrayBufferCopy(ctx, msg.data.data(), msg.data.size());
        JSValue args[2] = {
            JS_NewUint32(ctx, msg.connection),
            ab
        };
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, func);
    };

    s_states[ctx] = std::move(state);

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
}

void NetBindings::cleanup(JSContext* ctx) {
    auto it = s_states.find(ctx);
    if (it == s_states.end()) return;

    auto& s = it->second;
    // Drop JS refs (the service's callbacks reference this state but we'll
    // clear them below; if a late event slips through getState() it returns
    // nullptr so the lambda body is a no-op).
    if (ctx) {
        JS_FreeValue(ctx, s.onConnect);
        JS_FreeValue(ctx, s.onDisconnect);
        JS_FreeValue(ctx, s.onMessage);
    }

    // Detach the subscriber. The service thread will close any sockets it
    // owns and free the subscriber asynchronously.
    if (s.service && s.subscriber) {
        s.service->destroySubscriber(s.subscriber);
    }

    s_states.erase(it);
}

void NetBindings::poll(JSContext* ctx) {
    auto* s = getState(ctx);
    if (!s || !s->subscriber) return;
    s->subscriber->poll();
}

} // namespace bro::js
