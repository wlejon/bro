// =============================================================================
// bro_kws_c_abi.h — Pure C-ABI declarations for bro.kws
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_KWS_C_ABI_H
#define BRO_KWS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.kws.KwsStreamView ---
void* bro_KwsStreamView_create(void);
void  bro_KwsStreamView_destroy(void* self);
bool bro_KwsStreamView_get_active(void* self);
int32_t bro_KwsStreamView_enroll(void* self, const char* name, void* phonemeIds, void* policy);
int32_t bro_KwsStreamView_enrollFromAudio(void* self, const char* name, void* samples, void* policy);
int32_t bro_KwsStreamView_enrollFromClasses(void* self, const char* name, void* classIds, void* policy);
void* bro_KwsStreamView_inspect(void* self, const char* name);
bool bro_KwsStreamView_remove(void* self, const char* name);
void bro_KwsStreamView_clear(void* self);
void* bro_KwsStreamView_templates(void* self);
void bro_KwsStreamView_reset(void* self);
void bro_KwsStreamView_listen(void* self, void* opts);
void bro_KwsStreamView_stop(void* self);
void bro_KwsStreamView_suspend(void* self);
void bro_KwsStreamView_resume(void* self);
bool bro_KwsStreamView_isActive(void* self);
bool bro_KwsStreamView_isSuspended(void* self);
bool bro_KwsStreamView_isLoaded(void* self);
int32_t bro_KwsStreamView_sampleRate(void* self);
double bro_KwsStreamView_prefixProgress(void* self);
void* bro_KwsStreamView_progress(void* self);
void* bro_KwsStreamView_posterior(void* self, int32_t topK);
void* bro_KwsStreamView_stats(void* self);
void* bro_KwsStreamView_feed(void* self, void* samples);

// --- Namespace bro.kws ---
void bro_kws_init(void);
void bro_kws_load(void* opts);
void bro_kws_unload(void);
int32_t bro_kws_enroll(const char* name, void* phonemeIds, void* policy);
int32_t bro_kws_enrollFromAudio(const char* name, void* samples, void* policy);
int32_t bro_kws_enrollFromClasses(const char* name, void* classIds, void* policy);
void* bro_kws_inspect(const char* name);
bool bro_kws_remove(const char* name);
void bro_kws_clear(void);
void* bro_kws_templates(void);
void bro_kws_reset(void);
void bro_kws_listen(void* opts);
void bro_kws_stop(void);
void bro_kws_suspend(void);
void bro_kws_resume(void);
bool bro_kws_isActive(void);
bool bro_kws_isSuspended(void);
bool bro_kws_isLoaded(void);
int32_t bro_kws_sampleRate(void);
double bro_kws_prefixProgress(void);
void* bro_kws_progress(void);
void* bro_kws_posterior(int32_t topK);
void* bro_kws_stats(void);
void* bro_kws_feed(void* samples);

#ifdef __cplusplus
}
#endif

#endif // BRO_KWS_C_ABI_H
