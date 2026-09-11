// =============================================================================
// bro_tts_c_abi.h — Pure C-ABI declarations for bro.tts
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_TTS_C_ABI_H
#define BRO_TTS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.tts.KokoroModel ---
void* bro_KokoroModel_create(void);
void  bro_KokoroModel_destroy(void* self);
bool bro_KokoroModel_get_loaded(void* self);
const char* bro_KokoroModel_get_device(void* self);
void* bro_KokoroModel_encodePhonemes(void* self, const char* ipa);
void* bro_KokoroModel_loadVoice(void* self, const char* path);
void* bro_KokoroModel_createSession(void* self);

// --- Interface bro.tts.Voice ---
void* bro_Voice_create(void);
void  bro_Voice_destroy(void* self);
bool bro_Voice_get_loaded(void* self);
const char* bro_Voice_get_name(void* self);

// --- Interface bro.tts.KokoroSession ---
void* bro_KokoroSession_create(void);
void  bro_KokoroSession_destroy(void* self);
bool bro_KokoroSession_get_loaded(void* self);
void* bro_KokoroSession_synthesize(void* self, void* phonemes, void* voice, void* opts);
void bro_KokoroSession_reset(void* self);

// --- Interface bro.tts.QwenTtsModel ---
void* bro_QwenTtsModel_create(void);
void  bro_QwenTtsModel_destroy(void* self);
bool bro_QwenTtsModel_get_loaded(void* self);
const char* bro_QwenTtsModel_get_device(void* self);
const char* bro_QwenTtsModel_get_variant(void* self);
void* bro_QwenTtsModel_createSession(void* self);
void* bro_QwenTtsModel_encodeAudio(void* self, void* audio);
void* bro_QwenTtsModel_decodeCodes(void* self, void* codes);

// --- Interface bro.tts.QwenTtsSession ---
void* bro_QwenTtsSession_create(void);
void  bro_QwenTtsSession_destroy(void* self);
bool bro_QwenTtsSession_get_loaded(void* self);
const char* bro_QwenTtsSession_get_variant(void* self);
void* bro_QwenTtsSession_synthesize(void* self, const char* text, void* opts);
void bro_QwenTtsSession_reset(void* self);

// --- Interface bro.tts.SupertonicModel ---
void* bro_SupertonicModel_create(void);
void  bro_SupertonicModel_destroy(void* self);
bool bro_SupertonicModel_get_loaded(void* self);
const char* bro_SupertonicModel_get_device(void* self);
void* bro_SupertonicModel_loadVoiceStyle(void* self, const char* path);

// --- Interface bro.tts.SupertonicVoice ---
void* bro_SupertonicVoice_create(void);
void  bro_SupertonicVoice_destroy(void* self);
bool bro_SupertonicVoice_get_loaded(void* self);
const char* bro_SupertonicVoice_get_name(void* self);

// --- Interface bro.tts.SpeakerEncoder ---
void* bro_SpeakerEncoder_create(void);
void  bro_SpeakerEncoder_destroy(void* self);
bool bro_SpeakerEncoder_get_loaded(void* self);
const char* bro_SpeakerEncoder_get_device(void* self);
void* bro_SpeakerEncoder_embedSpeaker(void* self, void* audio, void* opts);

// --- Namespace bro.tts ---
void bro_tts_init(void);
void* bro_tts_loadKokoro(const char* dir, void* opts);
void* bro_tts_loadQwen(const char* dir, void* opts);
void* bro_tts_loadSupertonic(const char* dir, void* opts);
void* bro_tts_loadSpeakerEncoder(const char* dir, void* opts);
void* bro_tts_phonemize(const char* text, void* opts);
void bro_tts_setAssetRoot(const char* dir);
void bro_tts_setAssets(void* opts);
void* bro_tts_synthesize(void* model, void* textOrPhonemes, void* voiceOrOpts, void* opts);
void* bro_tts_synthesizeStream(void* model, void* textOrChunks, void* voiceOrOpts, void* opts);
void* bro_tts_decodeFrom(void* kokoro, void* voice, void* asr, void* F0, void* N, int32_t nPhonemes, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_TTS_C_ABI_H
