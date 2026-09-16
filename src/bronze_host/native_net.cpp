// native_net.cpp — C entry points behind bro.net over GameNetworkingSockets (net::NetService).

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "net/net_service.h"
#include "util/log.h"
#include "natives/net/native_net_decl.h"
#include "embed/embed.h"

#include "bronze_host/host_worker_msg.h"
#include "runtime/typed_array.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

#if BRO_WITH_NET

namespace {

static constexpr uint8_t kWireMagic = 0xB7;
enum WireType : uint8_t {
    kWireRaw   = 0x00,
    kWireClone = 0x01,
};
static constexpr size_t kWireHeaderSize = 2;

struct NetState {
    net::NetSubscriber* subscriber = nullptr;
    bool hosting = false;
    std::unordered_map<uint32_t, bool> connections;
    ev::Persistent* dispatcher = nullptr;
};

static NetState g_net;

net::NetSubscriber* getNetSubscriber() {
    auto* eng = hostEngine();
    if (!eng || !eng->netService()) {
        g_net.subscriber = nullptr;
        return nullptr;
    }
    if (!g_net.subscriber) {
        g_net.subscriber = eng->netService()->createSubscriber();
        g_net.subscriber->onHostResult = [](bool success) {
            g_net.hosting = success;
        };
        g_net.subscriber->onConnect = [](uint32_t conn) {
            g_net.connections[conn] = true;
            if (g_net.dispatcher && ev::isFunction(g_net.dispatcher->get())) {
                Value args[2] = { ev::fromUtf8("connect"), ev::fromDouble(conn) };
                ev::call(g_net.dispatcher->get(), ev::undefined(), args);
            }
        };
        g_net.subscriber->onDisconnect = [](uint32_t conn, int reason) {
            g_net.connections.erase(conn);
            if (g_net.dispatcher && ev::isFunction(g_net.dispatcher->get())) {
                Value args[3] = { ev::fromUtf8("disconnect"), ev::fromDouble(conn), ev::fromDouble(reason) };
                ev::call(g_net.dispatcher->get(), ev::undefined(), args);
            }
        };
        g_net.subscriber->onMessage = [](net::NetworkMessage&& msg) {
            if (msg.data.size() < kWireHeaderSize || msg.data[0] != kWireMagic) {
                LOG_WARN("[net] conn %u: dropping message with invalid wire magic (%zu bytes)",
                         msg.connection, msg.data.size());
                return;
            }
            if (g_net.dispatcher && ev::isFunction(g_net.dispatcher->get())) {
                Value payloadVal;
                if (msg.data[1] == kWireClone) {
                    Message m;
                    m.data.assign(msg.data.begin() + kWireHeaderSize, msg.data.end());
                    payloadVal = deserializeMessage(m);
                } else {
                    // A raw payload arrives as an ArrayBuffer: it is what
                    // distinguishes it from a clone (`data instanceof
                    // ArrayBuffer`, net-sync's own test), and TextDecoder,
                    // DataView and every typed-array constructor take it.
                    const uint8_t* payload = msg.data.data() + kWireHeaderSize;
                    size_t payloadLen = msg.data.size() - kWireHeaderSize;
                    payloadVal = ev::createArrayBuffer(std::span<const uint8_t>(payload, payloadLen));
                }
                Value args[4] = {
                    ev::fromUtf8("message"),
                    ev::fromDouble(msg.connection),
                    payloadVal,
                    ev::fromDouble(msg.channel)
                };
                ev::call(g_net.dispatcher->get(), ev::undefined(), args);
            }
        };
    }
    return g_net.subscriber;
}

static std::vector<int32_t> s_peersScratch;

}  // namespace

void pollNet() {
    auto* eng = hostEngine();
    if (!eng || !eng->netService()) {
        g_net.subscriber = nullptr;
        return;
    }
    if (auto* sub = getNetSubscriber()) {
        sub->poll();
    }
}

// Internal hook for js/net.js to register its event dispatcher
void bro_net_setDispatcher(uint64_t fnBits) {
    if (!g_net.dispatcher) g_net.dispatcher = new ev::Persistent();
    g_net.dispatcher->set(ev::fromBits(fnBits));
}

bool bro_net_isHosting() {
    return g_net.hosting;
}

bool bro_net_init() {
    return getNetSubscriber() != nullptr;
}

// The third argument of every send: `{reliable, channel, nodelay}`, a bare
// channel number, or the legacy boolean `reliable`. Channels clamp to the
// lane range rather than failing the send (docs/net-api.js). An array is a
// postMessage transfer list, which nothing on the wire can honour: that is a
// TypeError (and `false`) rather than an options bag with no fields, so the
// caller learns the value was NOT sent.
static bool parseSendOptions(uint64_t optsBits, net::SendOptions& opts, const char* who) {
    opts = net::SendOptions{};
    if (optsBits == 0) return true;
    Value optVal = ev::fromBits(optsBits);
    auto clampChannel = [](Value v) {
        int ch = static_cast<int>(ev::toDouble(v));
        if (ch < 0) ch = 0;
        if (ch >= net::kNetLaneCount) ch = net::kNetLaneCount - 1;
        return ch;
    };
    if (ev::isObject(optVal)) {
        const auto* hdr = optVal.asObject<bronze::HeapObjectHeader>();
        if (hdr && hdr->flags == bronze::HeapKind::Array) {
            ev::throwTypeError(std::string(who) + ": transfer lists are not supported over the network");
            return false;
        }
        Value relV = ev::getProperty(optVal, "reliable");
        if (!ev::isUndefined(relV) && !ev::isNull(relV)) opts.reliable = ev::toBool(relV);
        Value chanV = ev::getProperty(optVal, "channel");
        if (!ev::isUndefined(chanV) && !ev::isNull(chanV)) opts.channel = clampChannel(chanV);
        Value noDelayV = ev::getProperty(optVal, "nodelay");
        if (!ev::isUndefined(noDelayV) && !ev::isNull(noDelayV)) opts.nodelay = ev::toBool(noDelayV);
    } else if (ev::isNumber(optVal)) {
        opts.channel = clampChannel(optVal);
    } else if (ev::isBool(optVal)) {
        opts.reliable = ev::toBool(optVal);
    }
    return true;
}

// Raw bytes with the full option set; the generated `send` / `broadcast`
// natives carry only a channel, so js/net.js routes through these.
bool bro_net_sendRawOpts(int32_t peerId, const uint8_t* data, uint32_t data_len, uint64_t optsBits) {
    auto* sub = getNetSubscriber();
    if (!sub || !data) return false;
    net::SendOptions opts;
    if (!parseSendOptions(optsBits, opts, "send")) return false;
    std::vector<uint8_t> framed;
    framed.reserve(kWireHeaderSize + data_len);
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    framed.insert(framed.end(), data, data + data_len);
    sub->send(static_cast<uint32_t>(peerId), std::move(framed), opts);
    return true;
}

void bro_net_broadcastRawOpts(const uint8_t* data, uint32_t data_len, uint64_t optsBits) {
    auto* sub = getNetSubscriber();
    if (!sub || !data) return;
    net::SendOptions opts;
    if (!parseSendOptions(optsBits, opts, "broadcast")) return;
    std::vector<uint8_t> framed;
    framed.reserve(kWireHeaderSize + data_len);
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    framed.insert(framed.end(), data, data + data_len);
    sub->broadcast(std::move(framed), opts);
}

bool bro_net_sendCloneRaw(int32_t peerId, uint64_t valBits, uint64_t optsBits) {
    auto* sub = getNetSubscriber();
    if (!sub) return false;
    Value val = ev::fromBits(valBits);
    net::SendOptions opts;
    if (!parseSendOptions(optsBits, opts, "sendClone")) return false;
    std::vector<uint8_t> framed;
    Message msg;
    if (!serializeMessage(val, {}, msg)) {
        // serializeMessage has already thrown the TypeError naming the
        // offending value; a second throw would replace it with a vaguer one.
        return false;
    }
    if (!msg.transferredBuffers.empty() || !msg.transferredImages.empty()) {
        ev::throwTypeError("sendClone: cannot transfer buffers or images across network");
        return false;
    }
    framed.reserve(kWireHeaderSize + msg.data.size());
    framed.push_back(kWireMagic);
    framed.push_back(kWireClone);
    framed.insert(framed.end(), msg.data.begin(), msg.data.end());
    sub->send(static_cast<uint32_t>(peerId), std::move(framed), opts);
    return true;
}

void bro_net_broadcastCloneRaw(uint64_t valBits, uint64_t optsBits) {
    auto* sub = getNetSubscriber();
    if (!sub) return;
    Value val = ev::fromBits(valBits);
    net::SendOptions opts;
    if (!parseSendOptions(optsBits, opts, "broadcastClone")) return;
    std::vector<uint8_t> framed;
    Message msg;
    if (!serializeMessage(val, {}, msg)) return;  // it threw the TypeError
    if (!msg.transferredBuffers.empty() || !msg.transferredImages.empty()) {
        ev::throwTypeError("broadcastClone: cannot transfer buffers or images across network");
        return;
    }
    framed.reserve(kWireHeaderSize + msg.data.size());
    framed.push_back(kWireMagic);
    framed.push_back(kWireClone);
    framed.insert(framed.end(), msg.data.begin(), msg.data.end());
    sub->broadcast(std::move(framed), opts);
}

bool registerNatives_net(std::string* error);

bool registerNetNatives(std::string* error) {
    if (!registerNatives_net(error)) return false;
    if (!natives::fn("__bro_native.net._setDispatcher",
                       reinterpret_cast<void*>(&bro_net_setDispatcher),
                       "void", {"dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.isHosting",
                       reinterpret_cast<void*>(&bro_net_isHosting),
                       "bool", {}, error)) return false;
    if (!natives::fn("__bro_native.net.init",
                       reinterpret_cast<void*>(&bro_net_init),
                       "bool", {}, error)) return false;
    if (!natives::fn("__bro_native.net.sendClone",
                       reinterpret_cast<void*>(&bro_net_sendCloneRaw),
                       "bool", {"i32", "dynamic", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.broadcastClone",
                       reinterpret_cast<void*>(&bro_net_broadcastCloneRaw),
                       "void", {"dynamic", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.sendRawOpts",
                       reinterpret_cast<void*>(&bro_net_sendRawOpts),
                       "bool", {"i32", "u8[]", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.broadcastRawOpts",
                       reinterpret_cast<void*>(&bro_net_broadcastRawOpts),
                       "void", {"u8[]", "dynamic"}, error)) return false;
    return true;
}

#else  // !BRO_WITH_NET

void pollNet() {}
bool registerNetNatives(std::string*) { return true; }

#endif  // BRO_WITH_NET

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

#if BRO_WITH_NET

void bro_net_host(int32_t port, uint64_t callback) {
    auto* sub = getNetSubscriber();
    if (!sub) return;
    g_net.hosting = true;
    sub->host(static_cast<uint16_t>(port));
    if (callback != 0) {
        Value fn = ev::fromBits(callback);
        if (ev::isFunction(fn)) {
            Value arg = ev::fromBool(true);
            const Value args[1] = {arg};
            ev::call(fn, ev::undefined(), args);
        }
    }
}

void bro_net_unhost(void) {
    auto* sub = getNetSubscriber();
    if (sub) sub->closeHost();
    g_net.hosting = false;
    g_net.connections.clear();
}

int32_t bro_net_connect(const char* address, int32_t port, uint64_t callback) {
    auto* sub = getNetSubscriber();
    if (!sub) return 0;
    std::string addr = address ? address : "";
    if (port > 0 && addr.find(':') == std::string::npos) {
        addr += ":" + std::to_string(port);
    }
    sub->connect(addr);
    if (callback != 0) {
        Value fn = ev::fromBits(callback);
        if (ev::isFunction(fn)) {
            Value arg = ev::fromDouble(1.0);
            const Value args[1] = {arg};
            ev::call(fn, ev::undefined(), args);
        }
    }
    return 1;
}

void bro_net_disconnect(int32_t peerId) {
    auto* sub = getNetSubscriber();
    if (sub) sub->disconnect(static_cast<uint32_t>(peerId), 0);
    g_net.connections.erase(static_cast<uint32_t>(peerId));
}

void bro_net_disconnectAll(void) {
    auto* sub = getNetSubscriber();
    if (sub) {
        for (auto& [conn, _] : g_net.connections) {
            sub->disconnect(conn, 0);
        }
    }
    g_net.connections.clear();
}

void bro_net_send(int32_t peerId, const uint8_t* data, uint32_t data_len, int32_t channel) {
    auto* sub = getNetSubscriber();
    if (!sub || !data) return;
    bro::net::SendOptions opts;
    opts.channel = std::clamp(channel, 0, bro::net::kNetLaneCount - 1);
    std::vector<uint8_t> framed;
    framed.reserve(kWireHeaderSize + data_len);
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    framed.insert(framed.end(), data, data + data_len);
    sub->send(static_cast<uint32_t>(peerId), std::move(framed), opts);
}

void bro_net_broadcast(const uint8_t* data, uint32_t data_len, int32_t channel) {
    auto* sub = getNetSubscriber();
    if (!sub || !data) return;
    bro::net::SendOptions opts;
    opts.channel = std::clamp(channel, 0, bro::net::kNetLaneCount - 1);
    std::vector<uint8_t> framed;
    framed.reserve(kWireHeaderSize + data_len);
    framed.push_back(kWireMagic);
    framed.push_back(kWireRaw);
    framed.insert(framed.end(), data, data + data_len);
    sub->broadcast(std::move(framed), opts);
}

void bro_net_peers(bronze_native_buffer* out) {
    s_peersScratch.clear();
    s_peersScratch.reserve(g_net.connections.size());
    for (auto& [conn, _] : g_net.connections) {
        s_peersScratch.push_back(static_cast<int32_t>(conn));
    }
    out->data = s_peersScratch.empty() ? nullptr : s_peersScratch.data();
    out->length = static_cast<uint32_t>(s_peersScratch.size());
    out->release = nullptr;
}

const char* bro_net_getPeerAddress(int32_t /*peerId*/) {
    return natives::strResult("");
}

const char* bro_net_stats(void) {
    return natives::strResult("{\"ping\":0,\"packetLoss\":0,\"bytesSent\":0,\"bytesRecv\":0}");
}

const char* bro_net_getPeerStats(int32_t peerId) {
    auto* sub = getNetSubscriber();
    bro::net::ConnectionStats st;
    if (!sub || !sub->getConnectionStats(static_cast<uint32_t>(peerId), st)) {
        return natives::strResult("null");
    }
    std::string s = "{\"ping\":" + std::to_string(st.ping) +
                    ",\"packetLoss\":" + std::to_string(st.packetLoss) +
                    ",\"bytesSent\":" + std::to_string(st.bytesPerSecSent) +
                    ",\"bytesRecv\":" + std::to_string(st.bytesPerSecRecv) + "}";
    return natives::strResult(s);
}

void bro_net_setPeerSimulatedLoss(int32_t /*peerId*/, double /*chance*/, double /*latencyMin*/, double /*latencyMax*/) {
}

#else  // !BRO_WITH_NET

void bro_net_host(int32_t, uint64_t) {}
void bro_net_unhost(void) {}
int32_t bro_net_connect(const char*, int32_t, uint64_t) { return 0; }
void bro_net_disconnect(int32_t) {}
void bro_net_disconnectAll(void) {}
void bro_net_send(int32_t, const uint8_t*, uint32_t, int32_t) {}
void bro_net_broadcast(const uint8_t*, uint32_t, int32_t) {}
void bro_net_peers(bronze_native_buffer* out) { out->data = nullptr; out->length = 0; }
const char* bro_net_getPeerAddress(int32_t) { return ""; }
const char* bro_net_stats(void) { return "null"; }
const char* bro_net_getPeerStats(int32_t) { return "null"; }
void bro_net_setPeerSimulatedLoss(int32_t, double, double, double) {}

#endif  // BRO_WITH_NET

}  // extern "C"
