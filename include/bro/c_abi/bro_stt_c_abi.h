// =============================================================================
// bro_stt_c_abi.h — Pure C-ABI declarations for bro.stt
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_STT_C_ABI_H
#define BRO_STT_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.stt.WhisperTokenizer ---
void* bro_WhisperTokenizer_create(void);
void  bro_WhisperTokenizer_destroy(void* self);
bool bro_WhisperTokenizer_get_loaded(void* self);
void* bro_WhisperTokenizer_encode(void* self, const char* text);
const char* bro_WhisperTokenizer_decode(void* self, void* tokenIds);
void* bro_WhisperTokenizer_buildPrompt(void* self, void* opts);

// --- Interface bro.stt.WhisperModel ---
void* bro_WhisperModel_create(void);
void  bro_WhisperModel_destroy(void* self);
bool bro_WhisperModel_get_loaded(void* self);
const char* bro_WhisperModel_get_device(void* self);
void* bro_WhisperModel_transcribe(void* self, void* audio, void* promptOrOpts, void* opts);
void* bro_WhisperModel_createSession(void* self);

// --- Interface bro.stt.WhisperSession ---
void* bro_WhisperSession_create(void);
void  bro_WhisperSession_destroy(void* self);
bool bro_WhisperSession_get_loaded(void* self);
void* bro_WhisperSession_transcribe(void* self, void* audio, void* promptOrOpts, void* opts);
void bro_WhisperSession_reset(void* self);

// --- Interface bro.stt.ParakeetTokenizer ---
void* bro_ParakeetTokenizer_create(void);
void  bro_ParakeetTokenizer_destroy(void* self);
bool bro_ParakeetTokenizer_get_loaded(void* self);
void* bro_ParakeetTokenizer_encode(void* self, const char* text);
const char* bro_ParakeetTokenizer_decode(void* self, void* tokenIds);

// --- Interface bro.stt.ParakeetModel ---
void* bro_ParakeetModel_create(void);
void  bro_ParakeetModel_destroy(void* self);
bool bro_ParakeetModel_get_loaded(void* self);
const char* bro_ParakeetModel_get_device(void* self);
void* bro_ParakeetModel_transcribe(void* self, void* audio, void* opts);
void* bro_ParakeetModel_createSession(void* self);

// --- Interface bro.stt.ParakeetSession ---
void* bro_ParakeetSession_create(void);
void  bro_ParakeetSession_destroy(void* self);
bool bro_ParakeetSession_get_loaded(void* self);
void* bro_ParakeetSession_transcribe(void* self, void* audio, void* opts);
void bro_ParakeetSession_reset(void* self);

// --- Interface bro.stt.QwenAsrModel ---
void* bro_QwenAsrModel_create(void);
void  bro_QwenAsrModel_destroy(void* self);
bool bro_QwenAsrModel_get_loaded(void* self);
const char* bro_QwenAsrModel_get_device(void* self);
void* bro_QwenAsrModel_transcribe(void* self, void* audio, void* opts);
void* bro_QwenAsrModel_createSession(void* self);

// --- Interface bro.stt.QwenAsrSession ---
void* bro_QwenAsrSession_create(void);
void  bro_QwenAsrSession_destroy(void* self);
bool bro_QwenAsrSession_get_loaded(void* self);
void* bro_QwenAsrSession_transcribe(void* self, void* audio, void* opts);
void bro_QwenAsrSession_reset(void* self);

// --- Interface bro.stt.QwenAsrStream ---
void* bro_QwenAsrStream_create(void);
void  bro_QwenAsrStream_destroy(void* self);
bool bro_QwenAsrStream_get_loaded(void* self);
void bro_QwenAsrStream_feed(void* self, void* audio);
void* bro_QwenAsrStream_finish(void* self);

// --- Namespace bro.stt ---
void bro_stt_init(void);
void* bro_stt_loadWhisper(const char* dir, void* opts);
void* bro_stt_loadTokenizer(void* opts);
void* bro_stt_loadParakeet(const char* dir, void* opts);
void* bro_stt_loadParakeetTokenizer(const char* path, void* opts);
void* bro_stt_loadQwenAsr(const char* dir, void* opts);
void* bro_stt_loadQwenAsrStream(const char* dir, void* opts);
void* bro_stt_transcribe(void* model, void* audio, void* promptOrOpts, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_STT_C_ABI_H
