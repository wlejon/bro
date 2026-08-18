// Networking & Remote Transport bindings for the bronze host layer.
//
// Exposes:
// 1. bro.net (GameNetworkingSockets via bro::net::NetService)
// 2. WebSocket Web API client (RFC 6455 / WHATWG standard)
//
// Follows bronze GC rules strictly:
// - Payload structs are plain host memory, freed by handle finalizers.
// - Finalizers never touch the embed API / never own Persistents.
// - Persistents live on JS properties or static global root state.
// - Heap pointers from typedArrayInfo/arrayBufferInfo are consumed before any allocation.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "net/net_service.h"
#include "util/log.h"
#include "util/object_url.h"
#include "util/remote_asset.h"

#ifdef BRO_HAVE_CURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Wire framing for bro.net
// ---------------------------------------------------------------------------
static constexpr uint8_t kWireMagic = 0xB7;
enum WireType : uint8_t {
    kWireRaw   = 0x00,
    kWireClone = 0x01,
};
static constexpr size_t kWireHeaderSize = 2;

// ---------------------------------------------------------------------------
// bro.net State
// ---------------------------------------------------------------------------
struct NetGlobalState {
    net::NetSubscriber* subscriber = nullptr;
    bool hosting = false;
    bool hostPending = false;
    int connectsPending = 0;
    std::unordered_map<uint32_t, bool> connections;
    ev::Persistent netObj;
};

static NetGlobalState g_netState;

// ---------------------------------------------------------------------------
// WebSocket State & Types
// ---------------------------------------------------------------------------
struct HostWebSocket {
    uint32_t tag = kHostWebSocketTag;
    std::string url;
    std::string protocol;
    std::string binaryType = "blob"; // "blob" or "arraybuffer"
    int readyState = 0; // 0=CONNECTING, 1=OPEN, 2=CLOSING, 3=CLOSED
    double bufferedAmount = 0.0;
    int closeCode = 1000;
    std::string closeReason;
    bool wasClean = true;
    std::string errorMsg;
    bool openDispatched = false;
    bool closeDispatched = false;

#ifdef BRO_HAVE_CURL
    CURL* easy = nullptr;
#endif

    struct InboxMessage {
        std::vector<uint8_t> data;
        bool binary = false;
    };
    std::vector<InboxMessage> inbox;
    std::vector<uint8_t> partialData;
    bool partialBinary = false;
};

#ifdef BRO_HAVE_CURL
static CURLM* g_wsMulti = nullptr;
static size_t wsDummyWrite(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}
#endif

struct ActiveWsEntry {
    HostWebSocket* state = nullptr;
    ev::Persistent jsObj;
};
static std::vector<ActiveWsEntry> g_activeWebSockets;

void hostWebSocketDtor(void* p) {
    auto* ws = static_cast<HostWebSocket*>(p);
#ifdef BRO_HAVE_CURL
    if (ws->easy) {
        if (g_wsMulti) curl_multi_remove_handle(g_wsMulti, ws->easy);
        curl_easy_cleanup(ws->easy);
        ws->easy = nullptr;
    }
#endif
    delete ws;
}

HostWebSocket* webSocketOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* ws = static_cast<HostWebSocket*>(ev::handleData(v));
    if (!ws || ws->tag != kHostWebSocketTag) return nullptr;
    return ws;
}

// ---------------------------------------------------------------------------
// Helpers: Payload Bytes & Send Options
// ---------------------------------------------------------------------------
static bool appendPayloadBytes(Value data, std::vector<uint8_t>& out) {
    if (const HostBlob* blob = hostBlobOf(data)) {
        out.insert(out.end(), blob->bytes.begin(), blob->bytes.end());
        return true;
    }
    if (ev::isTypedArray(data)) {
        ev::TypedArrayInfo info = ev::typedArrayInfo(data);
        if (info && info.data) {
            out.insert(out.end(), info.data, info.data + info.byteLength);
            return true;
        }
    }
    if (ev::isArrayBuffer(data)) {
        ev::ArrayBufferInfo info = ev::arrayBufferInfo(data);
        if (info && info.data) {
            out.insert(out.end(), info.data, info.data + info.byteLength);
            return true;
        }
    }
    if (ev::isUndefined(data) || ev::isNull(data)) {
        return true;
    }
    std::string str = ev::toUtf8(data);
    out.insert(out.end(), str.begin(), str.end());
    return true;
}

static bool parseSendOptions(std::span<const Value> a, size_t optIndex, net::SendOptions& out) {
    if (a.size() <= optIndex || ev::isUndefined(a[optIndex]) || ev::isNull(a[optIndex])) {
        return true;
    }
    Value v = a[optIndex];
    if (ev::isObject(v)) {
        Value relV = ev::getProperty(v, "reliable");
        if (!ev::isUndefined(relV) && !ev::isNull(relV)) {
            out.reliable = ev::toBool(relV);
        }
        Value chanV = ev::getProperty(v, "channel");
        if (!ev::isUndefined(chanV) && !ev::isNull(chanV)) {
            int ch = static_cast<int>(ev::toDouble(chanV));
            if (ch < 0) ch = 0;
            if (ch >= net::kNetLaneCount) ch = net::kNetLaneCount - 1;
            out.channel = ch;
        }
        Value noDelayV = ev::getProperty(v, "nodelay");
        if (!ev::isUndefined(noDelayV) && !ev::isNull(noDelayV)) {
            out.nodelay = ev::toBool(noDelayV);
        }
        return true;
    }
    out.reliable = ev::toBool(v);
    return true;
}

// ---------------------------------------------------------------------------
// NetSubscriber Event Dispatches
// ---------------------------------------------------------------------------
static void dispatchNetConnect(uint32_t conn) {
    if (!ev::isObject(g_netState.netObj.get())) return;
    Value connVal = ev::fromDouble(conn);
    ev::Persistent connP(connVal);

    for (const char* prop : {"onConnect", "onconnect"}) {
        Value fn = ev::getProperty(g_netState.netObj.get(), prop);
        if (ev::isFunction(fn)) {
            ev::Persistent fnP(fn);
            Value arg = connP.get();
            ev::CallResult r = ev::call(fnP.get(), g_netState.netObj.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("bro.net.onConnect", r.value);
        }
    }

    for (ev::Persistent& handler : hostListSnapshot(g_netState.netObj, "__bronzeHostListeners_connect")) {
        if (ev::isFunction(handler.get())) {
            Value arg = connP.get();
            ev::CallResult r = ev::call(handler.get(), g_netState.netObj.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("bro.net connect listener", r.value);
        }
    }
}

static void dispatchNetDisconnect(uint32_t conn, int reason) {
    if (!ev::isObject(g_netState.netObj.get())) return;
    Value connVal = ev::fromDouble(conn);
    Value reasonVal = ev::fromDouble(reason);
    ev::Persistent connP(connVal);
    ev::Persistent reasonP(reasonVal);

    for (const char* prop : {"onDisconnect", "ondisconnect"}) {
        Value fn = ev::getProperty(g_netState.netObj.get(), prop);
        if (ev::isFunction(fn)) {
            ev::Persistent fnP(fn);
            Value args[2] = { connP.get(), reasonP.get() };
            ev::CallResult r = ev::call(fnP.get(), g_netState.netObj.get(), args);
            if (r.thrown) reportBronzeError("bro.net.onDisconnect", r.value);
        }
    }

    for (ev::Persistent& handler : hostListSnapshot(g_netState.netObj, "__bronzeHostListeners_disconnect")) {
        if (ev::isFunction(handler.get())) {
            Value args[2] = { connP.get(), reasonP.get() };
            ev::CallResult r = ev::call(handler.get(), g_netState.netObj.get(), args);
            if (r.thrown) reportBronzeError("bro.net disconnect listener", r.value);
        }
    }
}

static void dispatchNetMessage(net::NetworkMessage&& msg) {
    if (!ev::isObject(g_netState.netObj.get())) return;
    if (msg.data.size() < kWireHeaderSize || msg.data[0] != kWireMagic) {
        LOG_WARN("[bronze_host:net] conn %u: dropping message with unrecognized wire framing (%zu bytes)",
                 msg.connection, msg.data.size());
        return;
    }
    const uint8_t frameType = msg.data[1];
    if (frameType != kWireRaw) {
        LOG_WARN("[bronze_host:net] conn %u: dropping frame with unsupported type %u",
                 msg.connection, frameType);
        return;
    }

    Value ab = ev::createArrayBuffer(std::span<const uint8_t>(
        msg.data.data() + kWireHeaderSize,
        msg.data.size() - kWireHeaderSize
    ));
    ev::Persistent abP(ab);
    ev::Persistent connP(ev::fromDouble(msg.connection));
    ev::Persistent chanP(ev::fromDouble(msg.channel));

    for (const char* prop : {"onMessage", "onmessage"}) {
        Value fn = ev::getProperty(g_netState.netObj.get(), prop);
        if (ev::isFunction(fn)) {
            ev::Persistent fnP(fn);
            Value args[3] = { connP.get(), abP.get(), chanP.get() };
            ev::CallResult r = ev::call(fnP.get(), g_netState.netObj.get(), args);
            if (r.thrown) reportBronzeError("bro.net.onMessage", r.value);
        }
    }

    for (ev::Persistent& handler : hostListSnapshot(g_netState.netObj, "__bronzeHostListeners_message")) {
        if (ev::isFunction(handler.get())) {
            Value args[3] = { connP.get(), abP.get(), chanP.get() };
            ev::CallResult r = ev::call(handler.get(), g_netState.netObj.get(), args);
            if (r.thrown) reportBronzeError("bro.net message listener", r.value);
        }
    }
}

static net::NetSubscriber* getNetSubscriber() {
    if (g_netState.subscriber) return g_netState.subscriber;
    auto* eng = hostEngine();
    if (!eng || !eng->netService()) return nullptr;
    g_netState.subscriber = eng->netService()->createSubscriber();
    if (g_netState.subscriber) {
        g_netState.subscriber->onHostResult = [](bool success) {
            g_netState.hosting = success;
            g_netState.hostPending = false;
        };
        g_netState.subscriber->onConnectResult = [](bool success) {
            if (!success && g_netState.connectsPending > 0) {
                g_netState.connectsPending--;
            }
        };
        g_netState.subscriber->onConnect = [](uint32_t conn) {
            if (g_netState.connectsPending > 0) g_netState.connectsPending--;
            g_netState.connections[conn] = true;
            dispatchNetConnect(conn);
        };
        g_netState.subscriber->onDisconnect = [](uint32_t conn, int reason) {
            if (g_netState.connections.erase(conn) == 0 && g_netState.connectsPending > 0) {
                g_netState.connectsPending--;
            }
            dispatchNetDisconnect(conn, reason);
        };
        g_netState.subscriber->onMessage = [](net::NetworkMessage&& msg) {
            dispatchNetMessage(std::move(msg));
        };
    }
    return g_netState.subscriber;
}

// ---------------------------------------------------------------------------
// bro.net JS Method Implementations
// ---------------------------------------------------------------------------
static Value js_net_init(Value, std::span<const Value>) {
    return ev::fromBool(true);
}

static Value js_net_host(Value, std::span<const Value> a) {
    if (a.empty() || ev::isUndefined(a[0])) {
        return ev::throwTypeError("bro.net.host requires a port number");
    }
    int port = i32At(a, 0);
    if (port < 1 || port > 65535) {
        return ev::throwRangeError("port must be 1..65535");
    }
    auto* sub = getNetSubscriber();
    if (!sub) return ev::fromBool(false);
    g_netState.hostPending = true;
    sub->host(static_cast<uint16_t>(port));
    return ev::fromBool(true);
}

static Value js_net_connect(Value, std::span<const Value> a) {
    if (a.empty() || ev::isUndefined(a[0])) {
        return ev::throwTypeError("bro.net.connect requires an address string");
    }
    std::string addr = ev::toUtf8(a[0]);
    if (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
        int port = i32At(a, 1);
        if (port > 0 && addr.find(':') == std::string::npos) {
            addr = addr + ":" + std::to_string(port);
        }
    }
    auto* sub = getNetSubscriber();
    if (!sub) return ev::fromBool(false);
    g_netState.connectsPending++;
    sub->connect(addr);
    return ev::fromBool(true);
}

static Value js_net_disconnect(Value, std::span<const Value> a) {
    if (a.empty()) return ev::undefined();
    uint32_t conn = static_cast<uint32_t>(i32At(a, 0));
    int reason = 0;
    if (a.size() >= 2) reason = i32At(a, 1);
    if (auto* sub = getNetSubscriber()) {
        sub->disconnect(conn, reason);
    }
    g_netState.connections.erase(conn);
    return ev::undefined();
}

static Value js_net_send(Value, std::span<const Value> a) {
    if (a.size() < 2) return ev::fromBool(false);
    uint32_t conn = static_cast<uint32_t>(i32At(a, 0));
    net::SendOptions opts;
    if (!parseSendOptions(a, 2, opts)) return ev::fromBool(false);

    std::vector<uint8_t> framed;
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    if (!appendPayloadBytes(a[1], framed)) return ev::fromBool(false);

    if (auto* sub = getNetSubscriber()) {
        sub->send(conn, std::move(framed), opts);
        return ev::fromBool(true);
    }
    return ev::fromBool(false);
}

static Value js_net_broadcast(Value, std::span<const Value> a) {
    if (a.empty()) return ev::undefined();
    net::SendOptions opts;
    if (!parseSendOptions(a, 1, opts)) return ev::undefined();

    std::vector<uint8_t> framed;
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    if (!appendPayloadBytes(a[0], framed)) return ev::undefined();

    if (auto* sub = getNetSubscriber()) {
        sub->broadcast(std::move(framed), opts);
    }
    return ev::undefined();
}

static Value js_net_close(Value, std::span<const Value>) {
    if (auto* sub = getNetSubscriber()) {
        sub->closeHost();
    }
    g_netState.hosting = false;
    g_netState.connections.clear();
    return ev::undefined();
}

static Value js_net_isHosting(Value, std::span<const Value>) {
    return ev::fromBool(g_netState.hosting);
}

static Value js_net_connections(Value, std::span<const Value>) {
    std::vector<uint32_t> list;
    list.reserve(g_netState.connections.size());
    for (auto& [conn, _] : g_netState.connections) {
        list.push_back(conn);
    }
    return hostArrayOf(list.size(), [&list](size_t i) -> Value {
        return ev::fromDouble(list[i]);
    });
}

static Value js_net_stats(Value, std::span<const Value> a) {
    if (a.empty()) return ev::null();
    uint32_t conn = static_cast<uint32_t>(i32At(a, 0));
    auto* sub = getNetSubscriber();
    if (!sub) return ev::null();
    net::ConnectionStats st;
    if (!sub->getConnectionStats(conn, st)) return ev::null();

    ObjectBuilder b;
    b.set("ping", ev::fromDouble(st.ping));
    b.set("packetLoss", ev::fromDouble(st.packetLoss));
    b.set("bytesSent", ev::fromDouble(st.bytesPerSecSent));
    b.set("bytesRecv", ev::fromDouble(st.bytesPerSecRecv));
    b.set("bytesPerSecSent", ev::fromDouble(st.bytesPerSecSent));
    b.set("bytesPerSecRecv", ev::fromDouble(st.bytesPerSecRecv));
    return b.get();
}

static Value js_net_addEventListener(Value, std::span<const Value> a) {
    if (a.size() < 2) return ev::undefined();
    std::string type = ev::toUtf8(a[0]);
    if (type.rfind("on", 0) == 0) type = type.substr(2);
    addHostListener(g_netState.netObj, type, a[1]);
    return ev::undefined();
}

static Value js_net_removeEventListener(Value, std::span<const Value> a) {
    if (a.size() < 2) return ev::undefined();
    std::string type = ev::toUtf8(a[0]);
    if (type.rfind("on", 0) == 0) type = type.substr(2);
    removeHostListener(g_netState.netObj, type, a[1]);
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// WebSocket Event Dispatches
// ---------------------------------------------------------------------------
static void dispatchWsOpen(ev::Persistent& target) {
    ev::Persistent evt(ev::createObject());
    evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8("open")));
    evt.set(ev::setProperty(evt.get(), "target", target.get()));
    evt.set(ev::setProperty(evt.get(), "currentTarget", target.get()));

    Value on = ev::getProperty(target.get(), "onopen");
    if (ev::isFunction(on)) {
        ev::Persistent fnP(on);
        Value arg = evt.get();
        ev::CallResult r = ev::call(fnP.get(), target.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("WebSocket.onopen", r.value);
    }
    for (ev::Persistent& handler : hostListSnapshot(target, "__bronzeHostListeners_open")) {
        if (ev::isFunction(handler.get())) {
            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("WebSocket open listener", r.value);
        }
    }
}

static void dispatchWsMessage(ev::Persistent& target, const std::vector<uint8_t>& data, bool binary,
                              const std::string& url, const std::string& binaryType) {
    ev::Persistent evt(ev::createObject());
    evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8("message")));
    evt.set(ev::setProperty(evt.get(), "target", target.get()));
    evt.set(ev::setProperty(evt.get(), "currentTarget", target.get()));
    evt.set(ev::setProperty(evt.get(), "origin", ev::fromUtf8(url)));

    if (binary) {
        if (binaryType == "arraybuffer") {
            Value ab = ev::createArrayBuffer(std::span<const uint8_t>(data.data(), data.size()));
            evt.set(ev::setProperty(evt.get(), "data", ab));
        } else {
            Value blob = makeBlobValue(data, "");
            evt.set(ev::setProperty(evt.get(), "data", blob));
        }
    } else {
        std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        evt.set(ev::setProperty(evt.get(), "data", ev::fromUtf8(text)));
    }

    Value on = ev::getProperty(target.get(), "onmessage");
    if (ev::isFunction(on)) {
        ev::Persistent fnP(on);
        Value arg = evt.get();
        ev::CallResult r = ev::call(fnP.get(), target.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("WebSocket.onmessage", r.value);
    }
    for (ev::Persistent& handler : hostListSnapshot(target, "__bronzeHostListeners_message")) {
        if (ev::isFunction(handler.get())) {
            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("WebSocket message listener", r.value);
        }
    }
}

static void dispatchWsError(ev::Persistent& target, const std::string& msg) {
    ev::Persistent evt(ev::createObject());
    evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8("error")));
    evt.set(ev::setProperty(evt.get(), "target", target.get()));
    evt.set(ev::setProperty(evt.get(), "currentTarget", target.get()));
    evt.set(ev::setProperty(evt.get(), "message", ev::fromUtf8(msg)));

    Value on = ev::getProperty(target.get(), "onerror");
    if (ev::isFunction(on)) {
        ev::Persistent fnP(on);
        Value arg = evt.get();
        ev::CallResult r = ev::call(fnP.get(), target.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("WebSocket.onerror", r.value);
    }
    for (ev::Persistent& handler : hostListSnapshot(target, "__bronzeHostListeners_error")) {
        if (ev::isFunction(handler.get())) {
            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("WebSocket error listener", r.value);
        }
    }
}

static void dispatchWsClose(ev::Persistent& target, int code, const std::string& reason, bool wasClean) {
    ev::Persistent evt(ev::createObject());
    evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8("close")));
    evt.set(ev::setProperty(evt.get(), "target", target.get()));
    evt.set(ev::setProperty(evt.get(), "currentTarget", target.get()));
    evt.set(ev::setProperty(evt.get(), "code", ev::fromDouble(code)));
    evt.set(ev::setProperty(evt.get(), "reason", ev::fromUtf8(reason)));
    evt.set(ev::setProperty(evt.get(), "wasClean", ev::fromBool(wasClean)));

    Value on = ev::getProperty(target.get(), "onclose");
    if (ev::isFunction(on)) {
        ev::Persistent fnP(on);
        Value arg = evt.get();
        ev::CallResult r = ev::call(fnP.get(), target.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("WebSocket.onclose", r.value);
    }
    for (ev::Persistent& handler : hostListSnapshot(target, "__bronzeHostListeners_close")) {
        if (ev::isFunction(handler.get())) {
            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("WebSocket close listener", r.value);
        }
    }
}

// ---------------------------------------------------------------------------
// WebSocket JS Method Implementations
// ---------------------------------------------------------------------------
static Value js_ws_send(Value thisValue, std::span<const Value> a) {
    HostWebSocket* ws = webSocketOf(thisValue);
    if (!ws) return ev::throwTypeError("WebSocket.send: receiver is not a WebSocket");
    if (ws->readyState != 1) {
        return ev::throwError("InvalidStateError: WebSocket is not open");
    }
    if (a.empty() || ev::isUndefined(a[0])) return ev::undefined();

#ifdef BRO_HAVE_CURL
    if (ws->easy) {
        Value data = a[0];
        size_t sent = 0;
        if (const HostBlob* blob = hostBlobOf(data)) {
            curl_ws_send(ws->easy, blob->bytes.data(), blob->bytes.size(), &sent, 0, CURLWS_BINARY);
        } else if (ev::isTypedArray(data)) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(data);
            if (info && info.data) {
                curl_ws_send(ws->easy, info.data, info.byteLength, &sent, 0, CURLWS_BINARY);
            }
        } else if (ev::isArrayBuffer(data)) {
            ev::ArrayBufferInfo info = ev::arrayBufferInfo(data);
            if (info && info.data) {
                curl_ws_send(ws->easy, info.data, info.byteLength, &sent, 0, CURLWS_BINARY);
            }
        } else {
            std::string str = ev::toUtf8(data);
            curl_ws_send(ws->easy, str.data(), str.size(), &sent, 0, CURLWS_TEXT);
        }
    }
#endif
    return ev::undefined();
}

static Value js_ws_close(Value thisValue, std::span<const Value> a) {
    HostWebSocket* ws = webSocketOf(thisValue);
    if (!ws) return ev::throwTypeError("WebSocket.close: receiver is not a WebSocket");
    if (ws->readyState >= 2) return ev::undefined(); // already closing or closed

    int code = 1000;
    if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isNull(a[0])) {
        code = i32At(a, 0);
    }
    std::string reason;
    if (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
        reason = ev::toUtf8(a[1]);
    }

    ws->closeCode = code;
    ws->closeReason = reason;
    ws->readyState = 2; // CLOSING

#ifdef BRO_HAVE_CURL
    if (ws->easy) {
        std::vector<uint8_t> payload;
        payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(code & 0xFF));
        payload.insert(payload.end(), reason.begin(), reason.end());
        size_t sent = 0;
        curl_ws_send(ws->easy, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
    }
#endif
    ws->readyState = 3; // CLOSED
    return ev::undefined();
}

static HostClass g_webSocketClass;

// The whole WebSocket surface except its four handler slots: the readyState
// constants, five accessors over the payload, send/close, and the listener
// pair. Every one reads its RECEIVER, so one copy on the prototype serves
// every socket a program opens.
static void decorateWebSocketProto(ObjectBuilder& b) {
    b.set("CONNECTING", ev::fromDouble(0));
    b.set("OPEN", ev::fromDouble(1));
    b.set("CLOSING", ev::fromDouble(2));
    b.set("CLOSED", ev::fromDouble(3));

    b.accessor("url", [](Value thisValue, std::span<const Value>) {
        HostWebSocket* s = webSocketOf(thisValue);
        return ev::fromUtf8(s ? s->url : "");
    }, nullptr);

    b.accessor("readyState", [](Value thisValue, std::span<const Value>) {
        HostWebSocket* s = webSocketOf(thisValue);
        return ev::fromDouble(s ? s->readyState : 3);
    }, nullptr);

    b.accessor("protocol", [](Value thisValue, std::span<const Value>) {
        HostWebSocket* s = webSocketOf(thisValue);
        return ev::fromUtf8(s ? s->protocol : "");
    }, nullptr);

    b.accessor("binaryType", [](Value thisValue, std::span<const Value>) {
        HostWebSocket* s = webSocketOf(thisValue);
        return ev::fromUtf8(s ? s->binaryType : "blob");
    }, [](Value thisValue, std::span<const Value> a) {
        HostWebSocket* s = webSocketOf(thisValue);
        if (!s || a.empty()) return ev::undefined();
        std::string bt = ev::toUtf8(a[0]);
        if (bt == "blob" || bt == "arraybuffer") s->binaryType = bt;
        return ev::undefined();
    });

    b.accessor("bufferedAmount", [](Value thisValue, std::span<const Value>) {
        HostWebSocket* s = webSocketOf(thisValue);
        return ev::fromDouble(s ? s->bufferedAmount : 0.0);
    }, nullptr);

    b.def("send", 1, js_ws_send);
    b.def("close", 2, js_ws_close);

    b.def("addEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        if (a.size() < 2) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type.rfind("on", 0) == 0) type = type.substr(2);
        ev::Persistent self(thisValue);
        addHostListener(self, type, a[1]);
        return ev::undefined();
    });

    b.def("removeEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        if (a.size() < 2) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type.rfind("on", 0) == 0) type = type.substr(2);
        ev::Persistent self(thisValue);
        removeHostListener(self, type, a[1]);
        return ev::undefined();
    });
}

static Value webSocketCtor(Value, std::span<const Value> a) {
        if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
            return ev::throwTypeError("Failed to construct 'WebSocket': 1 argument required, but only 0 present.");
        }
        std::string url = ev::toUtf8(a[0]);
        std::string protocol;
        if (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
            protocol = ev::toUtf8(a[1]);
        }

        auto* ws = new HostWebSocket();
        ws->url = url;
        ws->protocol = protocol;
        ws->readyState = 0; // CONNECTING
        ws->binaryType = "blob";
        ws->bufferedAmount = 0.0;

        ObjectBuilder b(g_webSocketClass.make(ws, hostWebSocketDtor));

        // The only OWN properties: the four handler slots, present and null so
        // an assignment writes a property already in the shape.
        for (const char* name : {"onopen", "onmessage", "onerror", "onclose"}) {
            b.set(name, ev::null());
        }

        Value wsVal = b.get();
        ev::Persistent wsPersistent(wsVal);

#ifdef BRO_HAVE_CURL
        ws->easy = curl_easy_init();
        if (ws->easy) {
            std::string curlUrl = ws->url;
            if (curlUrl.rfind("ws://", 0) == 0) {
                curlUrl = "http://" + curlUrl.substr(5);
            } else if (curlUrl.rfind("wss://", 0) == 0) {
                curlUrl = "https://" + curlUrl.substr(6);
            }
            curl_easy_setopt(ws->easy, CURLOPT_URL, curlUrl.c_str());
            curl_easy_setopt(ws->easy, CURLOPT_CONNECT_ONLY, 2L);
            curl_easy_setopt(ws->easy, CURLOPT_WRITEFUNCTION, wsDummyWrite);
            curl_easy_setopt(ws->easy, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(ws->easy, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(ws->easy, CURLOPT_USERAGENT, "bro/1.0");

            if (!ws->protocol.empty()) {
                std::string hdr = "Sec-WebSocket-Protocol: " + ws->protocol;
                struct curl_slist* hdrs = curl_slist_append(nullptr, hdr.c_str());
                curl_easy_setopt(ws->easy, CURLOPT_HTTPHEADER, hdrs);
            }

            if (!g_wsMulti) g_wsMulti = curl_multi_init();
            curl_multi_add_handle(g_wsMulti, ws->easy);
        } else {
            ws->readyState = 3;
            ws->errorMsg = "curl_easy_init failed";
        }
#else
        ws->readyState = 3;
        ws->errorMsg = "No WebSocket transport compiled in";
#endif

        g_activeWebSockets.push_back({ ws, wsPersistent });
        return wsVal;
}

static void pumpWebSockets() {
#ifdef BRO_HAVE_CURL
    if (g_wsMulti) {
        int running = 0;
        curl_multi_perform(g_wsMulti, &running);
        CURLMsg* msg;
        int msgsInQueue = 0;
        while ((msg = curl_multi_info_read(g_wsMulti, &msgsInQueue))) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL* easy = msg->easy_handle;
            for (auto& entry : g_activeWebSockets) {
                if (entry.state->easy == easy) {
                    if (msg->data.result == CURLE_OK) {
                        entry.state->readyState = 1; // OPEN
                    } else {
                        entry.state->readyState = 3; // CLOSED
                        entry.state->wasClean = false;
                        entry.state->closeCode = 1006;
                        entry.state->errorMsg = curl_easy_strerror(msg->data.result);
                    }
                    break;
                }
            }
        }
    }
#endif

    for (auto it = g_activeWebSockets.begin(); it != g_activeWebSockets.end(); ) {
        HostWebSocket* ws = it->state;
        ev::Persistent target = it->jsObj;

        if (ws->readyState == 1 && !ws->openDispatched) {
            ws->openDispatched = true;
            dispatchWsOpen(target);
        }

#ifdef BRO_HAVE_CURL
        if (ws->easy && (ws->readyState == 1 || ws->readyState == 2)) {
            char buf[4096];
            for (int poll = 0; poll < 10; ++poll) {
                size_t nread = 0;
                const struct curl_ws_frame* meta = nullptr;
                CURLcode rc = curl_ws_recv(ws->easy, buf, sizeof(buf), &nread, &meta);
                if (rc == CURLE_AGAIN) break;
                if (rc != CURLE_OK) {
                    if (ws->readyState != 3) {
                        ws->readyState = 3;
                        if (ws->closeCode == 0) ws->closeCode = 1006;
                        ws->wasClean = false;
                        ws->errorMsg = std::string("curl_ws_recv: ") + curl_easy_strerror(rc);
                    }
                    break;
                }
                if (!meta) break;
                if (meta->flags & CURLWS_CLOSE) {
                    if (nread >= 2) {
                        ws->closeCode = (static_cast<uint8_t>(buf[0]) << 8) | static_cast<uint8_t>(buf[1]);
                        if (nread > 2) {
                            ws->closeReason.assign(buf + 2, nread - 2);
                        }
                    } else {
                        ws->closeCode = 1005;
                    }
                    ws->readyState = 3;
                    ws->wasClean = true;
                    break;
                }
                if (meta->flags & CURLWS_PING) {
                    size_t sent = 0;
                    curl_ws_send(ws->easy, buf, nread, &sent, 0, CURLWS_PONG);
                    continue;
                }
                if ((meta->flags & CURLWS_TEXT) || (meta->flags & CURLWS_BINARY) || (meta->flags & CURLWS_CONT)) {
                    bool isBin = (meta->flags & CURLWS_BINARY) != 0 || ws->partialBinary;
                    ws->partialData.insert(ws->partialData.end(), buf, buf + nread);
                    ws->partialBinary = isBin;
                    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                        ws->inbox.push_back({ std::move(ws->partialData), ws->partialBinary });
                        ws->partialData.clear();
                        ws->partialBinary = false;
                    }
                }
            }
        }
#endif

        while (!ws->inbox.empty()) {
            auto msg = std::move(ws->inbox.front());
            ws->inbox.erase(ws->inbox.begin());
            dispatchWsMessage(target, msg.data, msg.binary, ws->url, ws->binaryType);
        }

        if (ws->readyState == 3 && !ws->closeDispatched) {
            ws->closeDispatched = true;
            if (!ws->errorMsg.empty()) {
                dispatchWsError(target, ws->errorMsg);
            }
            dispatchWsClose(target, ws->closeCode, ws->closeReason, ws->wasClean);

#ifdef BRO_HAVE_CURL
            if (ws->easy) {
                if (g_wsMulti) curl_multi_remove_handle(g_wsMulti, ws->easy);
                curl_easy_cleanup(ws->easy);
                ws->easy = nullptr;
            }
#endif

            it = g_activeWebSockets.erase(it);
            continue;
        }

        ++it;
    }
}

} // namespace

Value makeBroNetValue() {
    ObjectBuilder b;
    b.def("init", 0, js_net_init);
    b.def("host", 1, js_net_host);
    b.def("connect", 2, js_net_connect);
    b.def("disconnect", 2, js_net_disconnect);
    b.def("send", 3, js_net_send);
    b.def("broadcast", 2, js_net_broadcast);
    b.def("close", 0, js_net_close);
    b.def("closeHost", 0, js_net_close);
    b.def("isHosting", 0, js_net_isHosting);
    b.def("connections", 0, js_net_connections);
    b.def("stats", 1, js_net_stats);
    b.def("addEventListener", 2, js_net_addEventListener);
    b.def("removeEventListener", 2, js_net_removeEventListener);

    for (const char* name : {"onConnect", "onconnect", "onDisconnect", "ondisconnect", "onMessage", "onmessage"}) {
        b.set(name, ev::null());
    }

    Value val = b.get();
    g_netState.netObj.set(val);
    return val;
}

void installNetGlobals() {
    g_webSocketClass.install("WebSocket", 1, webSocketCtor, decorateWebSocketProto);
    // The readyState constants on the constructor as well as the prototype,
    // which is where the web has them both.
    g_webSocketClass.setStatic("CONNECTING", ev::fromDouble(0));
    g_webSocketClass.setStatic("OPEN", ev::fromDouble(1));
    g_webSocketClass.setStatic("CLOSING", ev::fromDouble(2));
    g_webSocketClass.setStatic("CLOSED", ev::fromDouble(3));
    ev::registerGlobal("CloseEvent", makeBrandConstructor("CloseEvent"));
    ev::registerGlobal("MessageEvent", makeBrandConstructor("MessageEvent"));
}

void drainNetEvents() {
    if (auto* sub = getNetSubscriber()) {
        sub->poll();
    }
    pumpWebSockets();
}

} // namespace bro::bronze_host
