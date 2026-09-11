// =============================================================================
// bro_net_c_abi.h — Pure C-ABI declarations for bro.net
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_NET_C_ABI_H
#define BRO_NET_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.net ---
void bro_net_host(int32_t port, void* callback);
void bro_net_unhost(void);
int32_t bro_net_connect(const char* address, int32_t port, void* callback);
void bro_net_disconnect(int32_t peerId);
void bro_net_disconnectAll(void);
void bro_net_send(int32_t peerId, void* data, int32_t channel);
void bro_net_broadcast(void* data, int32_t channel);
void bro_net_sendClone(int32_t peerId, void* value, int32_t channel);
void bro_net_broadcastClone(void* value, int32_t channel);
void* bro_net_peers(void);
const char* bro_net_getPeerAddress(int32_t peerId);
void* bro_net_stats(void);
void* bro_net_getPeerStats(int32_t peerId);
void bro_net_setPeerSimulatedLoss(int32_t peerId, double chance, double latencyMin, double latencyMax);
void* bro_net_get_onconnect(void);
void bro_net_set_onconnect(void* val);
void* bro_net_get_ondisconnect(void);
void bro_net_set_ondisconnect(void* val);
void* bro_net_get_onmessage(void);
void bro_net_set_onmessage(void* val);

#ifdef __cplusplus
}
#endif

#endif // BRO_NET_C_ABI_H
