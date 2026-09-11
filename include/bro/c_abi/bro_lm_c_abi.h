// =============================================================================
// bro_lm_c_abi.h — Pure C-ABI declarations for bro.lm
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_LM_C_ABI_H
#define BRO_LM_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.lm.AsyncHandle ---
void* bro_AsyncHandle_create(void);
void  bro_AsyncHandle_destroy(void* self);
void bro_AsyncHandle_cancel(void* self);

// --- Interface bro.lm.QwenTokenizer ---
void* bro_QwenTokenizer_create(void);
void  bro_QwenTokenizer_destroy(void* self);
int32_t bro_QwenTokenizer_get_imEndId(void* self);
int32_t bro_QwenTokenizer_get_imStartId(void* self);
void* bro_QwenTokenizer_encode(void* self, const char* text);
const char* bro_QwenTokenizer_decode(void* self, void* ids);
const char* bro_QwenTokenizer_applyChatTemplate(void* self, void* messages, bool addGenerationPrompt);

// --- Interface bro.lm.MistralTokenizer ---
void* bro_MistralTokenizer_create(void);
void  bro_MistralTokenizer_destroy(void* self);
int32_t bro_MistralTokenizer_get_eosId(void* self);
int32_t bro_MistralTokenizer_get_bosId(void* self);
int32_t bro_MistralTokenizer_get_vocabCount(void* self);
void* bro_MistralTokenizer_encode(void* self, const char* text, bool addSpecial);
const char* bro_MistralTokenizer_decode(void* self, void* ids);
const char* bro_MistralTokenizer_applyChatTemplate(void* self, void* messages, bool addGenerationPrompt);

// --- Interface bro.lm.GemmaTokenizer ---
void* bro_GemmaTokenizer_create(void);
void  bro_GemmaTokenizer_destroy(void* self);
int32_t bro_GemmaTokenizer_get_eosId(void* self);
int32_t bro_GemmaTokenizer_get_bosId(void* self);
int32_t bro_GemmaTokenizer_get_padId(void* self);
int32_t bro_GemmaTokenizer_get_unkId(void* self);
int32_t bro_GemmaTokenizer_get_vocabCount(void* self);
void* bro_GemmaTokenizer_encode(void* self, const char* text, bool addBos);
const char* bro_GemmaTokenizer_decode(void* self, void* ids);

// --- Interface bro.lm.LMModel ---
void* bro_LMModel_create(void);
void  bro_LMModel_destroy(void* self);
const char* bro_LMModel_get_family(void* self);
int32_t bro_LMModel_get_vocabSize(void* self);
int32_t bro_LMModel_get_hiddenSize(void* self);
int32_t bro_LMModel_get_numLayers(void* self);
int32_t bro_LMModel_get_maxSeqLen(void* self);
int32_t bro_LMModel_get_cacheLen(void* self);
void bro_LMModel_allocateCache(void* self, int32_t maxTokens);
void bro_LMModel_resetCache(void* self);
void* bro_LMModel_generate(void* self, void* promptIds, void* opts);
void* bro_LMModel_generateStream(void* self, void* promptIds, void* opts, void* onToken);

// --- Interface bro.lm.Qwen35Model ---
void* bro_Qwen35Model_create(void);
void  bro_Qwen35Model_destroy(void* self);
const char* bro_Qwen35Model_get_family(void* self);
int32_t bro_Qwen35Model_get_vocabSize(void* self);
int32_t bro_Qwen35Model_get_hiddenSize(void* self);
int32_t bro_Qwen35Model_get_numLayers(void* self);
int32_t bro_Qwen35Model_get_maxSeqLen(void* self);
int32_t bro_Qwen35Model_get_eosId(void* self);
int32_t bro_Qwen35Model_get_imEndId(void* self);
int32_t bro_Qwen35Model_get_endoftextId(void* self);
void* bro_Qwen35Model_encode(void* self, const char* text, bool addSpecial);
const char* bro_Qwen35Model_decode(void* self, void* ids);
void* bro_Qwen35Model_generate(void* self, const char* prompt, void* opts);

// --- Interface bro.lm.Qwen3VLModel ---
void* bro_Qwen3VLModel_create(void);
void  bro_Qwen3VLModel_destroy(void* self);
const char* bro_Qwen3VLModel_get_family(void* self);
int32_t bro_Qwen3VLModel_get_vocabSize(void* self);
int32_t bro_Qwen3VLModel_get_hiddenSize(void* self);
int32_t bro_Qwen3VLModel_get_numLayers(void* self);
int32_t bro_Qwen3VLModel_get_maxSeqLen(void* self);
int32_t bro_Qwen3VLModel_get_eosId(void* self);
int32_t bro_Qwen3VLModel_get_imEndId(void* self);
int32_t bro_Qwen3VLModel_get_endoftextId(void* self);
void* bro_Qwen3VLModel_encode(void* self, const char* text, bool addSpecial);
const char* bro_Qwen3VLModel_decode(void* self, void* ids);
void* bro_Qwen3VLModel_generate(void* self, const char* prompt, void* opts);

// --- Interface bro.lm.NllbModel ---
void* bro_NllbModel_create(void);
void  bro_NllbModel_destroy(void* self);
const char* bro_NllbModel_get_family(void* self);
int32_t bro_NllbModel_get_vocabSize(void* self);
int32_t bro_NllbModel_get_dModel(void* self);
int32_t bro_NllbModel_get_encoderLayers(void* self);
int32_t bro_NllbModel_get_decoderLayers(void* self);
int32_t bro_NllbModel_get_languageCount(void* self);
bool bro_NllbModel_hasLanguage(void* self, const char* code);
void* bro_NllbModel_translate(void* self, const char* text, const char* srcLang, const char* tgtLang, void* opts);

// --- Interface bro.lm.ClipModel ---
void* bro_ClipModel_create(void);
void  bro_ClipModel_destroy(void* self);
int32_t bro_ClipModel_get_projectionDim(void* self);
void* bro_ClipModel_encodeText(void* self, void* text);
void* bro_ClipModel_encodeImage(void* self, void* image);
void* bro_ClipModel_score(void* self, void* text, void* image);

// --- Interface bro.lm.T5Model ---
void* bro_T5Model_create(void);
void  bro_T5Model_destroy(void* self);
int32_t bro_T5Model_get_dModel(void* self);
int32_t bro_T5Model_get_maxLength(void* self);
int32_t bro_T5Model_get_padId(void* self);
int32_t bro_T5Model_get_eosId(void* self);
int32_t bro_T5Model_get_vocabCount(void* self);
void* bro_T5Model_encode(void* self, const char* text, void* opts);

// --- Namespace bro.lm ---
void bro_lm_init(void);
void* bro_lm_loadQwen(const char* ggufPath, void* opts);
void* bro_lm_loadMistral(const char* ggufPath, void* opts);
void* bro_lm_loadGemma2(const char* modelDir, void* opts);
void* bro_lm_loadQwen35(const char* checkpointDir, void* opts);
void* bro_lm_loadQwen3VL(const char* checkpointDir, void* opts);
void* bro_lm_loadNllb(const char* checkpointDir, void* opts);
void* bro_lm_loadTokenizer(void* opts);
void* bro_lm_loadClip(void* opts);
void* bro_lm_loadT5(void* opts);
void* bro_lm_generate(void* model, void* prompt, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_LM_C_ABI_H
