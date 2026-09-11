// =============================================================================
// bro_net_c_abi.cpp — C++ forwarding implementations for bro.net
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_net_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static void* s_net_onconnect = nullptr;
static void* s_net_ondisconnect = nullptr;
static void* s_net_onmessage = nullptr;

void bro_net_host(int32_t port, void* callback) {
    const auto* b = bro_get_net_bridge();
    if (b && b->host) b->host(port, callback);
}

void bro_net_unhost(void) {
    const auto* b = bro_get_net_bridge();
    if (b && b->unhost) b->unhost();
}

int32_t bro_net_connect(const char* address, int32_t port, void* callback) {
    const auto* b = bro_get_net_bridge();
    if (b && b->connect) return b->connect(address, port, callback);
    return 0;
}

void bro_net_disconnect(int32_t peerId) {
    const auto* b = bro_get_net_bridge();
    if (b && b->disconnect) b->disconnect(peerId);
}

void bro_net_disconnectAll(void) {
    const auto* b = bro_get_net_bridge();
    if (b && b->disconnectAll) b->disconnectAll();
}

void bro_net_send(int32_t peerId, void* data, int32_t channel) {
    const auto* b = bro_get_net_bridge();
    if (b && b->send) b->send(peerId, data, channel);
}

void bro_net_broadcast(void* data, int32_t channel) {
    const auto* b = bro_get_net_bridge();
    if (b && b->broadcast) b->broadcast(data, channel);
}

void bro_net_sendClone(int32_t peerId, void* value, int32_t channel) {
    const auto* b = bro_get_net_bridge();
    if (b && b->sendClone) b->sendClone(peerId, value, channel);
}

void bro_net_broadcastClone(void* value, int32_t channel) {
    const auto* b = bro_get_net_bridge();
    if (b && b->broadcastClone) b->broadcastClone(value, channel);
}

void* bro_net_peers(void) {
    const auto* b = bro_get_net_bridge();
    if (b && b->peers) return b->peers();
    return nullptr;
}

const char* bro_net_getPeerAddress(int32_t peerId) {
    const auto* b = bro_get_net_bridge();
    if (b && b->getPeerAddress) return b->getPeerAddress(peerId);
    return "";
}

void* bro_net_stats(void) {
    const auto* b = bro_get_net_bridge();
    if (b && b->stats) return b->stats();
    return nullptr;
}

void* bro_net_getPeerStats(int32_t peerId) {
    const auto* b = bro_get_net_bridge();
    if (b && b->getPeerStats) return b->getPeerStats(peerId);
    return nullptr;
}

void bro_net_setPeerSimulatedLoss(int32_t peerId, double chance, double latencyMin, double latencyMax) {
    const auto* b = bro_get_net_bridge();
    if (b && b->setPeerSimulatedLoss) b->setPeerSimulatedLoss(peerId, chance, latencyMin, latencyMax);
}

void* bro_net_get_onconnect(void) { return s_net_onconnect; }
void bro_net_set_onconnect(void* val) { s_net_onconnect = val; }

void* bro_net_get_ondisconnect(void) { return s_net_ondisconnect; }
void bro_net_set_ondisconnect(void* val) { s_net_ondisconnect = val; }

void* bro_net_get_onmessage(void) { return s_net_onmessage; }
void bro_net_set_onmessage(void* val) { s_net_onmessage = val; }

} // extern "C"
