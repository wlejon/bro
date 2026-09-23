// native_net.cpp — C entry points behind bro.net over GameNetworkingSockets (net::NetService).

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#if BRO_WITH_NET
// net_service.h pulls in GameNetworkingSockets, which a build without
// BRO_WITH_NET does not have (src/net is not even configured); the stubs
// below the gate need none of it.
#include "net/net_service.h"
#include <steam/isteamnetworkingutils.h>
#endif
#include "util/log.h"
#include "natives/net/native_net_decl.h"
#include "embed/embed.h"
#include "abi/bronze_abi.h"

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

// One per JS realm: the main document and each Worker own a NetSubscriber
// of their own (net_service.h: a subscriber is a per-thread handle whose
// callbacks fire on the thread that polls it) and the dispatcher their own
// js/net.js handed over, a Persistent in that thread's slot table.
//
// A trivially-destructible thread_local POINTER, never a thread_local
// object: a NetState with a destructor made the CRT register a dynamic TLS
// destructor for every thread in the process, and short-lived driver threads
// (NVIDIA's GL probes) ran ~Persistent after bronze's own TLS was gone
// (ee8505cf). The state is allocated on first use and released by
// releaseNetState() on the thread that owns it, or leaked with the main
// thread at exit, which is what the process teardown wants.
struct NetState {
    net::NetSubscriber* subscriber = nullptr;
    bool hosting = false;
    std::unordered_map<uint32_t, bool> connections;
    ev::Persistent* dispatcher = nullptr;
    std::vector<double> peersScratch;
};

static thread_local NetState* t_net = nullptr;

static NetState& netState() {
    if (!t_net) t_net = new NetState();
    return *t_net;
}

net::NetSubscriber* getNetSubscriber() {
    NetState& g_net = netState();
    auto* eng = hostEngine();
    if (!eng || !eng->netService()) {
        g_net.subscriber = nullptr;
        return nullptr;
    }
    if (!g_net.subscriber) {
        // The callbacks fire from poll() on this same thread, so each reads
        // the state back through netState() rather than capturing it.
        g_net.subscriber = eng->netService()->createSubscriber();
        g_net.subscriber->onHostResult = [](bool success) {
            netState().hosting = success;
        };
        g_net.subscriber->onConnect = [](uint32_t conn) {
            NetState& g_net = netState();
            g_net.connections[conn] = true;
            if (g_net.dispatcher && ev::isFunction(g_net.dispatcher->get())) {
                Value args[2] = { ev::fromUtf8("connect"), ev::fromDouble(conn) };
                ev::call(g_net.dispatcher->get(), ev::undefined(), args);
            }
        };
        g_net.subscriber->onDisconnect = [](uint32_t conn, int reason) {
            NetState& g_net = netState();
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
            if (msg.data[1] != kWireRaw && msg.data[1] != kWireClone) {
                LOG_WARN("[net] conn %u: dropping message with unknown wire type 0x%02x (%zu bytes)",
                         msg.connection, msg.data[1], msg.data.size());
                return;
            }
            NetState& g_net = netState();
            if (g_net.dispatcher && ev::isFunction(g_net.dispatcher->get())) {
                Value payloadVal;
                if (msg.data[1] == kWireClone) {
                    Message m;
                    m.data.assign(msg.data.begin() + kWireHeaderSize, msg.data.end());
                    payloadVal = deserializeMessage(m);
                    if (bronze_exception_pending()) {
                        bronze_exception_take();
                        LOG_WARN("[net] conn %u: dropping malformed clone message (%zu bytes)",
                                 msg.connection, msg.data.size());
                        return;
                    }
                } else {
                    // A raw payload arrives as an ArrayBuffer: it is what
                    // distinguishes it from a clone (`data instanceof
                    // ArrayBuffer`, net-sync's own test), and TextDecoder,
                    // DataView and every typed-array constructor take it.
                    const uint8_t* payload = msg.data.data() + kWireHeaderSize;
                    size_t payloadLen = msg.data.size() - kWireHeaderSize;
                    payloadVal = ev::createArrayBuffer(std::span<const uint8_t>(payload, payloadLen));
                }
                // The payload is rooted before the argument values are made:
                // fromUtf8 (and a boxed fromDouble) allocate, which would
                // leave a raw payloadVal pointing at the old semispace.
                ev::Persistent payload(payloadVal);
                ev::Persistent type(ev::fromUtf8("message"));
                ev::Persistent conn(ev::fromDouble(msg.connection));
                ev::Persistent chan(ev::fromDouble(msg.channel));
                Value args[4] = {type.get(), conn.get(), payload.get(), chan.get()};
                ev::call(g_net.dispatcher->get(), ev::undefined(), args);
            }
        };
    }
    return g_net.subscriber;
}

}  // namespace

// Polls the subscriber this thread already has; it does not mint one, so a
// Worker that never touches bro.net never registers with the service. Every
// command (host, connect, init, ...) mints it on the way in.
void pollNet() {
    if (!t_net || !t_net->subscriber) return;
    auto* eng = hostEngine();
    if (!eng || !eng->netService()) {
        t_net->subscriber = nullptr;
        return;
    }
    t_net->subscriber->poll();
}

// A worker's realm is going away: hand the subscriber back to the service
// (its sockets close, nothing polls it again) and drop the dispatcher while
// this thread's Persistent slots still exist. Engine::stopBackgroundServices
// joins every worker before it resets the service, so the service is alive
// here.
void releaseNetState() {
    NetState* st = t_net;
    if (!st) return;
    t_net = nullptr;
    auto* eng = hostEngine();
    if (st->subscriber && eng && eng->netService()) {
        eng->netService()->destroySubscriber(st->subscriber);
    }
    delete st->dispatcher;
    delete st;
}

// Internal hook for js/net.js to register its event dispatcher
void bro_net_setDispatcher(uint64_t fnBits) {
    NetState& g_net = netState();
    if (!g_net.dispatcher) g_net.dispatcher = new ev::Persistent();
    g_net.dispatcher->set(ev::fromBits(fnBits));
}

bool bro_net_isHosting() {
    return netState().hosting;
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

bool bro_net_sendUnframed(int32_t peerId, const uint8_t* data, uint32_t data_len, uint64_t optsBits) {
    auto* sub = getNetSubscriber();
    if (!sub || !data) return false;
    net::SendOptions opts;
    if (!parseSendOptions(optsBits, opts, "_sendUnframed")) return false;
    std::vector<uint8_t> payload(data, data + data_len);
    return sub->send(static_cast<uint32_t>(peerId), std::move(payload), opts);
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
    if (!natives::fn("__bro_native.net._sendUnframed",
                       reinterpret_cast<void*>(&bro_net_sendUnframed),
                       "bool", {"i32", "u8[]", "dynamic"}, error)) return false;
    return true;
}

#else  // !BRO_WITH_NET

void pollNet() {}
void releaseNetState() {}
bool registerNetNatives(std::string*) { return true; }

#endif  // BRO_WITH_NET

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

#if BRO_WITH_NET

void bro_net_host(int32_t port, uint64_t callback) {
    auto* sub = getNetSubscriber();
    if (!sub) return;
    netState().hosting = true;
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
    NetState& g_net = netState();
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

void bro_net_disconnect(int32_t peerId, int32_t reason) {
    auto* sub = getNetSubscriber();
    if (sub) sub->disconnect(static_cast<uint32_t>(peerId), reason);
    netState().connections.erase(static_cast<uint32_t>(peerId));
}

void bro_net_disconnectAll(void) {
    auto* sub = getNetSubscriber();
    NetState& g_net = netState();
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

// f64[], not i32[]: a connection id is the full uint32 GNS handle, and the
// events hand it out as a number, so a top-bit id read back through an
// Int32Array would be negative and never `===` the one onconnect gave.
void bro_net_peers(bronze_native_buffer* out) {
    NetState& g_net = netState();
    std::vector<double>& scratch = g_net.peersScratch;
    scratch.clear();
    scratch.reserve(g_net.connections.size());
    for (auto& [conn, _] : g_net.connections) {
        scratch.push_back(static_cast<double>(conn));
    }
    out->data = scratch.empty() ? nullptr : scratch.data();
    out->length = static_cast<uint32_t>(scratch.size());
    out->release = nullptr;
}

const char* bro_net_getPeerAddress(int32_t peerId) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return natives::strResult("");
    SteamNetConnectionInfo_t info;
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(peerId), &info)) {
        return natives::strResult("");
    }
    char buf[128];
    info.m_addrRemote.ToString(buf, sizeof(buf), true);
    return natives::strResult(buf);
}

const char* bro_net_stats(void) {
    auto* sub = getNetSubscriber();
    NetState& g_net = netState();
    if (!sub || g_net.connections.empty()) {
        return natives::strResult("{\"ping\":0,\"packetLoss\":0,\"bytesSent\":0,\"bytesRecv\":0}");
    }
    float totalPing = 0.0f;
    float totalLoss = 0.0f;
    float totalBytesSent = 0.0f;
    float totalBytesRecv = 0.0f;
    int count = 0;
    for (auto& [conn, _] : g_net.connections) {
        bro::net::ConnectionStats st;
        if (sub->getConnectionStats(conn, st)) {
            totalPing += st.ping;
            totalLoss += st.packetLoss;
            totalBytesSent += st.bytesPerSecSent;
            totalBytesRecv += st.bytesPerSecRecv;
            count++;
        }
    }
    float avgPing = count > 0 ? (totalPing / count) : 0.0f;
    float avgLoss = count > 0 ? (totalLoss / count) : 0.0f;
    std::string s = "{\"ping\":" + std::to_string(avgPing) +
                    ",\"packetLoss\":" + std::to_string(avgLoss) +
                    ",\"bytesSent\":" + std::to_string(totalBytesSent) +
                    ",\"bytesRecv\":" + std::to_string(totalBytesRecv) + "}";
    return natives::strResult(s);
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

void bro_net_setPeerSimulatedLoss(int32_t peerId, double chance, double latencyMin, double latencyMax) {
    auto* utils = SteamNetworkingUtils();
    if (!utils) return;
    HSteamNetConnection conn = static_cast<HSteamNetConnection>(peerId);
    float lossPct = static_cast<float>(chance <= 1.0 && chance > 0.0 ? chance * 100.0 : chance);
    if (!utils->SetConnectionConfigValueFloat(conn, k_ESteamNetworkingConfig_FakePacketLoss_Send, lossPct)) {
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, lossPct);
    }
    if (!utils->SetConnectionConfigValueFloat(conn, k_ESteamNetworkingConfig_FakePacketLoss_Recv, lossPct)) {
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, lossPct);
    }
    if (latencyMin > 0.0 || latencyMax > 0.0) {
        int32_t lagMs = static_cast<int32_t>(latencyMin);
        if (!utils->SetConnectionConfigValueInt32(conn, k_ESteamNetworkingConfig_FakePacketLag_Send, lagMs)) {
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, lagMs);
        }
        if (!utils->SetConnectionConfigValueInt32(conn, k_ESteamNetworkingConfig_FakePacketLag_Recv, lagMs)) {
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, lagMs);
        }
    }
}

#else  // !BRO_WITH_NET

void bro_net_host(int32_t, uint64_t) {}
void bro_net_unhost(void) {}
int32_t bro_net_connect(const char*, int32_t, uint64_t) { return 0; }
void bro_net_disconnect(int32_t, int32_t) {}
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
