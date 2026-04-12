#include "js/net_bindings.h"
#include "net/network_manager.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::js {

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static net::NetworkManager* s_mgr = nullptr;

// Per-connection JS callback objects: { onmessage, ondisconnect }
// Stored as DupValue'd JSValues keyed by connection handle.
struct JSConnectionCallbacks {
    JSValue onmessage = JS_UNDEFINED;    // function(data: ArrayBuffer)
    JSValue ondisconnect = JS_UNDEFINED; // function(reason: number)
};
static std::unordered_map<HSteamNetConnection, JSConnectionCallbacks> s_connCallbacks;
static JSContext* s_ctx = nullptr;

// Host-level callbacks
static JSValue s_onConnect = JS_UNDEFINED;    // function(connId: number)
static JSValue s_onDisconnect = JS_UNDEFINED; // function(connId: number, reason: number)
static JSValue s_onMessage = JS_UNDEFINED;    // function(connId: number, data: ArrayBuffer)

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
// ---------------------------------------------------------------------------

static JSValue js_net_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_mgr) return JS_FALSE;
    return JS_NewBool(ctx, s_mgr->init());
}

// ---------------------------------------------------------------------------
// bro.net.host(port) → boolean
// ---------------------------------------------------------------------------

static JSValue js_net_host(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || !s_mgr->isInitialized()) {
        return JS_ThrowInternalError(ctx, "Network not initialized — call bro.net.init() first");
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "host() requires a port number");

    int32_t port = 0;
    JS_ToInt32(ctx, &port, argv[0]);
    if (port < 1 || port > 65535) return JS_ThrowRangeError(ctx, "port must be 1..65535");

    return JS_NewBool(ctx, s_mgr->host(static_cast<uint16_t>(port)));
}

// ---------------------------------------------------------------------------
// bro.net.connect(address) → boolean
// ---------------------------------------------------------------------------

static JSValue js_net_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || !s_mgr->isInitialized()) {
        return JS_ThrowInternalError(ctx, "Network not initialized — call bro.net.init() first");
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "connect() requires an address string");

    std::string addr = jsToString(ctx, argv[0]);
    return JS_NewBool(ctx, s_mgr->connect(addr));
}

// ---------------------------------------------------------------------------
// bro.net.send(connId, data, reliable?) → boolean
//   data: ArrayBuffer or string
//   reliable: boolean (default true)
// ---------------------------------------------------------------------------

static JSValue js_net_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || argc < 2) return JS_FALSE;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    bool reliable = true;
    if (argc >= 3) reliable = JS_ToBool(ctx, argv[2]);

    // Check if data is a string or ArrayBuffer
    if (JS_IsString(argv[1])) {
        std::string str = jsToString(ctx, argv[1]);
        return JS_NewBool(ctx, s_mgr->send(
            static_cast<HSteamNetConnection>(conn),
            str.data(), static_cast<uint32_t>(str.size()), reliable));
    }

    // ArrayBuffer
    size_t size = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf) {
        // Try typed array / DataView
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &blen, nullptr);
        if (!JS_IsException(abuf)) {
            buf = JS_GetArrayBuffer(ctx, &size, abuf);
            JS_FreeValue(ctx, abuf);
            if (buf) {
                buf += offset;
                size = blen;
            }
        } else {
            JS_FreeValue(ctx, abuf);
            return JS_ThrowTypeError(ctx, "send() data must be a string, ArrayBuffer, or TypedArray");
        }
    }

    if (!buf) return JS_FALSE;
    return JS_NewBool(ctx, s_mgr->send(
        static_cast<HSteamNetConnection>(conn),
        buf, static_cast<uint32_t>(size), reliable));
}

// ---------------------------------------------------------------------------
// bro.net.broadcast(data, reliable?) → undefined
// ---------------------------------------------------------------------------

static JSValue js_net_broadcast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || argc < 1) return JS_UNDEFINED;

    bool reliable = true;
    if (argc >= 2) reliable = JS_ToBool(ctx, argv[1]);

    if (JS_IsString(argv[0])) {
        std::string str = jsToString(ctx, argv[0]);
        s_mgr->broadcast(str.data(), static_cast<uint32_t>(str.size()), reliable);
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
            if (buf) {
                buf += offset;
                size = blen;
            }
        } else {
            JS_FreeValue(ctx, abuf);
        }
    }

    if (buf) {
        s_mgr->broadcast(buf, static_cast<uint32_t>(size), reliable);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.net.disconnect(connId, reason?) → undefined
// ---------------------------------------------------------------------------

static JSValue js_net_disconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || argc < 1) return JS_UNDEFINED;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    int reason = 0;
    if (argc >= 2) JS_ToInt32(ctx, &reason, argv[1]);

    s_mgr->disconnect(static_cast<HSteamNetConnection>(conn), reason);

    // Clean up per-connection JS callbacks
    auto it = s_connCallbacks.find(static_cast<HSteamNetConnection>(conn));
    if (it != s_connCallbacks.end()) {
        JS_FreeValue(ctx, it->second.onmessage);
        JS_FreeValue(ctx, it->second.ondisconnect);
        s_connCallbacks.erase(it);
    }

    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.net.close() → undefined   (close listen socket / stop hosting)
// ---------------------------------------------------------------------------

static JSValue js_net_close(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (s_mgr) s_mgr->closeHost();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.net.stats(connId) → { ping, packetLoss, bytesSent, bytesRecv } | null
// ---------------------------------------------------------------------------

static JSValue js_net_stats(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_mgr || argc < 1) return JS_NULL;

    uint32_t conn = 0;
    JS_ToUint32(ctx, &conn, argv[0]);

    net::ConnectionStats stats;
    if (!s_mgr->getConnectionStats(static_cast<HSteamNetConnection>(conn), stats))
        return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ping", JS_NewFloat64(ctx, stats.ping));
    JS_SetPropertyStr(ctx, obj, "packetLoss", JS_NewFloat64(ctx, stats.packetLoss));
    JS_SetPropertyStr(ctx, obj, "bytesSent", JS_NewFloat64(ctx, stats.bytesPerSecSent));
    JS_SetPropertyStr(ctx, obj, "bytesRecv", JS_NewFloat64(ctx, stats.bytesPerSecRecv));
    return obj;
}

// ---------------------------------------------------------------------------
// bro.net.connections() → [connId, ...]
// ---------------------------------------------------------------------------

static JSValue js_net_connections(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_mgr) return JS_NewArray(ctx);

    auto conns = s_mgr->connections();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < conns.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewUint32(ctx, static_cast<uint32_t>(conns[i])));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// bro.net.isHosting() → boolean
// ---------------------------------------------------------------------------

static JSValue js_net_isHosting(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, s_mgr && s_mgr->isHosting());
}

// ---------------------------------------------------------------------------
// Callback properties: bro.net.onconnect, onmessage, ondisconnect
// ---------------------------------------------------------------------------

static JSValue js_net_get_onconnect(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_DupValue(ctx, s_onConnect);
}
static JSValue js_net_set_onconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JS_FreeValue(ctx, s_onConnect);
    s_onConnect = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue js_net_get_ondisconnect(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_DupValue(ctx, s_onDisconnect);
}
static JSValue js_net_set_ondisconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JS_FreeValue(ctx, s_onDisconnect);
    s_onDisconnect = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue js_net_get_onmessage(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_DupValue(ctx, s_onMessage);
}
static JSValue js_net_set_onmessage(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JS_FreeValue(ctx, s_onMessage);
    s_onMessage = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

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
// Install / Cleanup
// ---------------------------------------------------------------------------

void NetBindings::install(JSContext* ctx, net::NetworkManager* mgr) {
    s_mgr = mgr;
    s_ctx = ctx;

    // Wire C++ callbacks → JS callbacks
    mgr->onConnect = [](HSteamNetConnection conn) {
        if (!s_ctx || JS_IsUndefined(s_onConnect)) return;
        JSValue arg = JS_NewUint32(s_ctx, static_cast<uint32_t>(conn));
        JSValue ret = JS_Call(s_ctx, s_onConnect, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(s_ctx, ret);
        JS_FreeValue(s_ctx, arg);
    };

    mgr->onDisconnect = [](HSteamNetConnection conn, int reason) {
        if (!s_ctx) return;
        // Fire global ondisconnect
        if (!JS_IsUndefined(s_onDisconnect)) {
            JSValue args[2] = {
                JS_NewUint32(s_ctx, static_cast<uint32_t>(conn)),
                JS_NewInt32(s_ctx, reason)
            };
            JSValue ret = JS_Call(s_ctx, s_onDisconnect, JS_UNDEFINED, 2, args);
            JS_FreeValue(s_ctx, ret);
            JS_FreeValue(s_ctx, args[0]);
            JS_FreeValue(s_ctx, args[1]);
        }
        // Clean up per-connection callbacks
        auto it = s_connCallbacks.find(conn);
        if (it != s_connCallbacks.end()) {
            JS_FreeValue(s_ctx, it->second.onmessage);
            JS_FreeValue(s_ctx, it->second.ondisconnect);
            s_connCallbacks.erase(it);
        }
    };

    mgr->onMessage = [](net::NetworkMessage&& msg) {
        if (!s_ctx || JS_IsUndefined(s_onMessage)) return;

        // Create an ArrayBuffer from the message data
        JSValue ab = JS_NewArrayBufferCopy(s_ctx, msg.data.data(), msg.data.size());
        JSValue args[2] = {
            JS_NewUint32(s_ctx, static_cast<uint32_t>(msg.connection)),
            ab
        };
        JSValue ret = JS_Call(s_ctx, s_onMessage, JS_UNDEFINED, 2, args);
        JS_FreeValue(s_ctx, ret);
        JS_FreeValue(s_ctx, args[0]);
        JS_FreeValue(s_ctx, args[1]);
    };

    // Create bro.net object using qjsbind::Global for bro, then attach net
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue netObj = JS_NewObject(ctx);

    // Methods via function list
    JS_SetPropertyFunctionList(ctx, netObj, js_net_funcs,
                               sizeof(js_net_funcs) / sizeof(js_net_funcs[0]));

    // Callback properties via getter/setter
    JSAtom onconnectAtom = JS_NewAtom(ctx, "onconnect");
    JS_DefinePropertyGetSet(ctx, netObj, onconnectAtom,
        JS_NewCFunction(ctx, js_net_get_onconnect, "get onconnect", 0),
        JS_NewCFunction(ctx, js_net_set_onconnect, "set onconnect", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, onconnectAtom);

    JSAtom ondisconnectAtom = JS_NewAtom(ctx, "ondisconnect");
    JS_DefinePropertyGetSet(ctx, netObj, ondisconnectAtom,
        JS_NewCFunction(ctx, js_net_get_ondisconnect, "get ondisconnect", 0),
        JS_NewCFunction(ctx, js_net_set_ondisconnect, "set ondisconnect", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, ondisconnectAtom);

    JSAtom onmessageAtom = JS_NewAtom(ctx, "onmessage");
    JS_DefinePropertyGetSet(ctx, netObj, onmessageAtom,
        JS_NewCFunction(ctx, js_net_get_onmessage, "get onmessage", 0),
        JS_NewCFunction(ctx, js_net_set_onmessage, "set onmessage", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, onmessageAtom);

    JS_SetPropertyStr(ctx, broObj, "net", netObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void NetBindings::cleanup(JSContext* ctx) {
    // Free JS callback references
    if (ctx) {
        JS_FreeValue(ctx, s_onConnect);
        JS_FreeValue(ctx, s_onDisconnect);
        JS_FreeValue(ctx, s_onMessage);
        for (auto& [conn, cbs] : s_connCallbacks) {
            JS_FreeValue(ctx, cbs.onmessage);
            JS_FreeValue(ctx, cbs.ondisconnect);
        }
    }
    s_onConnect = JS_UNDEFINED;
    s_onDisconnect = JS_UNDEFINED;
    s_onMessage = JS_UNDEFINED;
    s_connCallbacks.clear();
    s_ctx = nullptr;
    s_mgr = nullptr;
}

} // namespace bro::js
