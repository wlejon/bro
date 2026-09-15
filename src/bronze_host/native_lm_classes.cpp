#include "native_lm_internal.h"
#include <cstring>

using namespace bro::bronze_host;

extern "C" {

// --- Interface bro.lm.AsyncHandle ---
void bro_lm_AsyncHandle_dtor(void* self) {
    delete static_cast<BroAsyncHandleImpl*>(self);
}

void* bro_lm_AsyncHandle_ctor(void) {
    return new BroAsyncHandleImpl();
}

void bro_lm_AsyncHandle_cancel(void* self) {
    auto* h = static_cast<BroAsyncHandleImpl*>(self);
    if (h) h->cancelled = true;
}

// --- Interface bro.lm.QwenTokenizer ---
void bro_lm_QwenTokenizer_dtor(void* self) {
    delete static_cast<BroQwenTokenizerImpl*>(self);
}

void* bro_lm_QwenTokenizer_ctor(void) {
    return new BroQwenTokenizerImpl();
}

int32_t bro_lm_QwenTokenizer_imEndId_get(void* self) {
    auto* t = static_cast<BroQwenTokenizerImpl*>(self);
    return t ? t->imEndId : 151645;
}

int32_t bro_lm_QwenTokenizer_imStartId_get(void* self) {
    auto* t = static_cast<BroQwenTokenizerImpl*>(self);
    return t ? t->imStartId : 151644;
}

void bro_lm_QwenTokenizer_encode(void* /*self*/, const char* /*text*/, bronze_native_buffer* out) {
    if (out) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
    }
}

const char* bro_lm_QwenTokenizer_decode(void* /*self*/, const int32_t* /*ids*/, uint32_t /*ids_len*/) {
    return "";
}

const char* bro_lm_QwenTokenizer_applyChatTemplate(void* /*self*/, const char* /*messages*/,
                                                  bool /*addGenerationPrompt_given*/, bool /*addGenerationPrompt*/) {
    return "";
}

// --- Interface bro.lm.MistralTokenizer ---
void bro_lm_MistralTokenizer_dtor(void* self) {
    delete static_cast<BroMistralTokenizerImpl*>(self);
}

void* bro_lm_MistralTokenizer_ctor(void) {
    return new BroMistralTokenizerImpl();
}

int32_t bro_lm_MistralTokenizer_eosId_get(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->eosId : 2;
}

int32_t bro_lm_MistralTokenizer_bosId_get(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->bosId : 1;
}

int32_t bro_lm_MistralTokenizer_vocabCount_get(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->vocabCount : 32768;
}

void bro_lm_MistralTokenizer_encode(void* /*self*/, const char* /*text*/,
                                    bool /*addSpecial_given*/, bool /*addSpecial*/,
                                    bronze_native_buffer* out) {
    if (out) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
    }
}

const char* bro_lm_MistralTokenizer_decode(void* /*self*/, const int32_t* /*ids*/, uint32_t /*ids_len*/) {
    return "";
}

const char* bro_lm_MistralTokenizer_applyChatTemplate(void* /*self*/, const char* /*messages*/,
                                                     bool /*addGenerationPrompt_given*/, bool /*addGenerationPrompt*/) {
    return "";
}

// --- Interface bro.lm.GemmaTokenizer ---
void bro_lm_GemmaTokenizer_dtor(void* self) {
    delete static_cast<BroGemmaTokenizerImpl*>(self);
}

void* bro_lm_GemmaTokenizer_ctor(void) {
    return new BroGemmaTokenizerImpl();
}

int32_t bro_lm_GemmaTokenizer_eosId_get(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->eosId : 1;
}

int32_t bro_lm_GemmaTokenizer_bosId_get(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->bosId : 2;
}

int32_t bro_lm_GemmaTokenizer_padId_get(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->padId : 0;
}

int32_t bro_lm_GemmaTokenizer_unkId_get(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->unkId : 3;
}

int32_t bro_lm_GemmaTokenizer_vocabCount_get(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->vocabCount : 256000;
}

void bro_lm_GemmaTokenizer_encode(void* /*self*/, const char* /*text*/,
                                  bool /*addBos_given*/, bool /*addBos*/,
                                  bronze_native_buffer* out) {
    if (out) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
    }
}

const char* bro_lm_GemmaTokenizer_decode(void* /*self*/, const int32_t* /*ids*/, uint32_t /*ids_len*/) {
    return "";
}

// --- Interface bro.lm.LMModel ---
void bro_lm_LMModel_dtor(void* self) {
    delete static_cast<BroLMModelImpl*>(self);
}

void* bro_lm_LMModel_ctor(void) {
    return new BroLMModelImpl();
}

const char* bro_lm_LMModel_family_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->family.c_str() : "qwen3";
}

int32_t bro_lm_LMModel_vocabSize_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_lm_LMModel_hiddenSize_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_lm_LMModel_numLayers_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_lm_LMModel_maxSeqLen_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_lm_LMModel_cacheLen_get(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->cacheLen : 0;
}

void bro_lm_LMModel_allocateCache(void* self, int32_t maxTokens) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    if (m) m->cacheLen = maxTokens;
}

void bro_lm_LMModel_resetCache(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    if (m) m->cacheLen = 0;
}

// --- Interface bro.lm.Qwen35Model ---
void bro_lm_Qwen35Model_dtor(void* self) {
    delete static_cast<BroQwen35ModelImpl*>(self);
}

void* bro_lm_Qwen35Model_ctor(void) {
    return new BroQwen35ModelImpl();
}

const char* bro_lm_Qwen35Model_family_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->family.c_str() : "qwen35";
}

int32_t bro_lm_Qwen35Model_vocabSize_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_lm_Qwen35Model_hiddenSize_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_lm_Qwen35Model_numLayers_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_lm_Qwen35Model_maxSeqLen_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_lm_Qwen35Model_eosId_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->eosId : 151645;
}

int32_t bro_lm_Qwen35Model_imEndId_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->imEndId : 151645;
}

int32_t bro_lm_Qwen35Model_endoftextId_get(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->endoftextId : 151643;
}

void bro_lm_Qwen35Model_encode(void* /*self*/, const char* /*text*/,
                               bool /*addSpecial_given*/, bool /*addSpecial*/,
                               bronze_native_buffer* out) {
    if (out) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
    }
}

const char* bro_lm_Qwen35Model_decode(void* /*self*/, const int32_t* /*ids*/, uint32_t /*ids_len*/) {
    return "";
}

// --- Interface bro.lm.Qwen3VLModel ---
void bro_lm_Qwen3VLModel_dtor(void* self) {
    delete static_cast<BroQwen3VLModelImpl*>(self);
}

void* bro_lm_Qwen3VLModel_ctor(void) {
    return new BroQwen3VLModelImpl();
}

const char* bro_lm_Qwen3VLModel_family_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->family.c_str() : "qwen3vl";
}

int32_t bro_lm_Qwen3VLModel_vocabSize_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_lm_Qwen3VLModel_hiddenSize_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_lm_Qwen3VLModel_numLayers_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_lm_Qwen3VLModel_maxSeqLen_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_lm_Qwen3VLModel_eosId_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->eosId : 151645;
}

int32_t bro_lm_Qwen3VLModel_imEndId_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->imEndId : 151645;
}

int32_t bro_lm_Qwen3VLModel_endoftextId_get(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->endoftextId : 151643;
}

void bro_lm_Qwen3VLModel_encode(void* /*self*/, const char* /*text*/,
                                bool /*addSpecial_given*/, bool /*addSpecial*/,
                                bronze_native_buffer* out) {
    if (out) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
    }
}

const char* bro_lm_Qwen3VLModel_decode(void* /*self*/, const int32_t* /*ids*/, uint32_t /*ids_len*/) {
    return "";
}

// --- Interface bro.lm.NllbModel ---
void bro_lm_NllbModel_dtor(void* self) {
    delete static_cast<BroNllbModelImpl*>(self);
}

void* bro_lm_NllbModel_ctor(void) {
    return new BroNllbModelImpl();
}

const char* bro_lm_NllbModel_family_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->family.c_str() : "nllb";
}

int32_t bro_lm_NllbModel_vocabSize_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->vocabSize : 256206;
}

int32_t bro_lm_NllbModel_dModel_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->dModel : 1024;
}

int32_t bro_lm_NllbModel_encoderLayers_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->encoderLayers : 12;
}

int32_t bro_lm_NllbModel_decoderLayers_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->decoderLayers : 12;
}

int32_t bro_lm_NllbModel_languageCount_get(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->languageCount : 200;
}

bool bro_lm_NllbModel_hasLanguage(void* /*self*/, const char* /*code*/) {
    return true;
}

// --- Interface bro.lm.ClipModel ---
void bro_lm_ClipModel_dtor(void* self) {
    delete static_cast<BroClipModelImpl*>(self);
}

void* bro_lm_ClipModel_ctor(void) {
    return new BroClipModelImpl();
}

int32_t bro_lm_ClipModel_projectionDim_get(void* self) {
    auto* m = static_cast<BroClipModelImpl*>(self);
    return m ? m->projectionDim : 768;
}

// --- Interface bro.lm.T5Model ---
void bro_lm_T5Model_dtor(void* self) {
    delete static_cast<BroT5ModelImpl*>(self);
}

void* bro_lm_T5Model_ctor(void) {
    return new BroT5ModelImpl();
}

int32_t bro_lm_T5Model_dModel_get(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->dModel : 4096;
}

int32_t bro_lm_T5Model_maxLength_get(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->maxLength : 512;
}

int32_t bro_lm_T5Model_padId_get(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->padId : 0;
}

int32_t bro_lm_T5Model_eosId_get(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->eosId : 1;
}

int32_t bro_lm_T5Model_vocabCount_get(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->vocabCount : 32128;
}

static thread_local T5EncodeSlot tl_t5EncodeSlot;

void bro_lm_T5Model_encode(void* /*self*/, const char* /*text*/, bool /*opts_maxLength_given*/, int32_t opts_maxLength) {
    tl_t5EncodeSlot.data.clear();
    tl_t5EncodeSlot.length = opts_maxLength > 0 ? opts_maxLength : 512;
    tl_t5EncodeSlot.dim = 4096;
    tl_t5EncodeSlot.ids.clear();
}

void bro_lm_T5Model_encode_data(bronze_native_buffer* out) {
    if (out) {
        out->data = tl_t5EncodeSlot.data.data();
        out->length = static_cast<uint32_t>(tl_t5EncodeSlot.data.size());
        out->release = nullptr;
    }
}

int32_t bro_lm_T5Model_encode_length(void) {
    return tl_t5EncodeSlot.length;
}

int32_t bro_lm_T5Model_encode_dim(void) {
    return tl_t5EncodeSlot.dim;
}

void bro_lm_T5Model_encode_ids(bronze_native_buffer* out) {
    if (out) {
        out->data = tl_t5EncodeSlot.ids.data();
        out->length = static_cast<uint32_t>(tl_t5EncodeSlot.ids.size());
        out->release = nullptr;
    }
}

}  // extern "C"
