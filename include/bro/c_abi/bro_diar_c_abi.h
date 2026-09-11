// =============================================================================
// bro_diar_c_abi.h — Pure C-ABI declarations for bro.diar
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_DIAR_C_ABI_H
#define BRO_DIAR_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.diar ---
void bro_diar_init(void);
void* bro_diar_loadSortformer(const char* modelDir, void* opts);
void* bro_diar_diarize(void* model, void* audio, void* opts);
void* bro_diar_loadClusterDiarizer(const char* embeddingDir, const char* vadDir, void* opts);
void* bro_diar_clusterDiarize(void* model, void* audio, void* opts);

// --- Interface bro.diar.Sortformer ---
void* bro_Sortformer_create(void);
void  bro_Sortformer_destroy(void* self);
void* bro_Sortformer_diarize(void* self, void* audio);
void* bro_Sortformer_createSession(void* self);
const char* bro_Sortformer_get_device(void* self);
bool bro_Sortformer_get_busy(void* self);

// --- Interface bro.diar.SortformerSession ---
void* bro_SortformerSession_create(void);
void  bro_SortformerSession_destroy(void* self);
void* bro_SortformerSession_feed(void* self, void* audio, bool isLast);
void bro_SortformerSession_reset(void* self);
const char* bro_SortformerSession_get_device(void* self);
bool bro_SortformerSession_get_busy(void* self);

// --- Interface bro.diar.ClusterDiarizer ---
void* bro_ClusterDiarizer_create(void);
void  bro_ClusterDiarizer_destroy(void* self);
void* bro_ClusterDiarizer_diarize(void* self, void* audio, void* opts);
const char* bro_ClusterDiarizer_get_device(void* self);
bool bro_ClusterDiarizer_get_busy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_DIAR_C_ABI_H
