// =============================================================================
// bro_lm_c_abi.cpp — C++ forwarding implementations for bro.lm
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_lm_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroAsyncHandleImpl {
    bool cancelled = false;
};

struct BroQwenTokenizerImpl {
    int32_t imEndId = 151645;
    int32_t imStartId = 151644;
};

struct BroMistralTokenizerImpl {
    int32_t eosId = 2;
    int32_t bosId = 1;
    int32_t vocabCount = 32768;
};

struct BroGemmaTokenizerImpl {
    int32_t eosId = 1;
    int32_t bosId = 2;
    int32_t padId = 0;
    int32_t unkId = 3;
    int32_t vocabCount = 256000;
};

struct BroLMModelImpl {
    std::string family = "qwen3";
    int32_t vocabSize = 151936;
    int32_t hiddenSize = 4096;
    int32_t numLayers = 32;
    int32_t maxSeqLen = 4096;
    int32_t cacheLen = 0;
};

struct BroQwen35ModelImpl {
    std::string family = "qwen35";
    int32_t vocabSize = 151936;
    int32_t hiddenSize = 4096;
    int32_t numLayers = 32;
    int32_t maxSeqLen = 4096;
    int32_t eosId = 151645;
    int32_t imEndId = 151645;
    int32_t endoftextId = 151643;
};

struct BroQwen3VLModelImpl {
    std::string family = "qwen3vl";
    int32_t vocabSize = 151936;
    int32_t hiddenSize = 4096;
    int32_t numLayers = 32;
    int32_t maxSeqLen = 4096;
    int32_t eosId = 151645;
    int32_t imEndId = 151645;
    int32_t endoftextId = 151643;
};

struct BroNllbModelImpl {
    std::string family = "nllb";
    int32_t vocabSize = 256206;
    int32_t dModel = 1024;
    int32_t encoderLayers = 12;
    int32_t decoderLayers = 12;
    int32_t languageCount = 200;
};

struct BroClipModelImpl {
    int32_t projectionDim = 768;
};

struct BroT5ModelImpl {
    int32_t dModel = 4096;
    int32_t maxLength = 512;
    int32_t padId = 0;
    int32_t eosId = 1;
    int32_t vocabCount = 32128;
};

extern "C" {

// --- Interface bro.lm.AsyncHandle ---
void* bro_AsyncHandle_create(void) {
    return new BroAsyncHandleImpl();
}

void bro_AsyncHandle_destroy(void* self) {
    delete static_cast<BroAsyncHandleImpl*>(self);
}

void bro_AsyncHandle_cancel(void* self) {
    auto* h = static_cast<BroAsyncHandleImpl*>(self);
    if (h) h->cancelled = true;
}

// --- Interface bro.lm.QwenTokenizer ---
void* bro_QwenTokenizer_create(void) {
    return new BroQwenTokenizerImpl();
}

void bro_QwenTokenizer_destroy(void* self) {
    delete static_cast<BroQwenTokenizerImpl*>(self);
}

int32_t bro_QwenTokenizer_get_imEndId(void* self) {
    auto* t = static_cast<BroQwenTokenizerImpl*>(self);
    return t ? t->imEndId : 151645;
}

int32_t bro_QwenTokenizer_get_imStartId(void* self) {
    auto* t = static_cast<BroQwenTokenizerImpl*>(self);
    return t ? t->imStartId : 151644;
}

void* bro_QwenTokenizer_encode(void* /*self*/, const char* /*text*/) {
    return nullptr;
}

const char* bro_QwenTokenizer_decode(void* /*self*/, void* /*ids*/) {
    return "";
}

const char* bro_QwenTokenizer_applyChatTemplate(void* /*self*/, void* /*messages*/, bool /*addGenerationPrompt*/) {
    return "";
}

// --- Interface bro.lm.MistralTokenizer ---
void* bro_MistralTokenizer_create(void) {
    return new BroMistralTokenizerImpl();
}

void bro_MistralTokenizer_destroy(void* self) {
    delete static_cast<BroMistralTokenizerImpl*>(self);
}

int32_t bro_MistralTokenizer_get_eosId(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->eosId : 2;
}

int32_t bro_MistralTokenizer_get_bosId(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->bosId : 1;
}

int32_t bro_MistralTokenizer_get_vocabCount(void* self) {
    auto* t = static_cast<BroMistralTokenizerImpl*>(self);
    return t ? t->vocabCount : 32768;
}

void* bro_MistralTokenizer_encode(void* /*self*/, const char* /*text*/, bool /*addSpecial*/) {
    return nullptr;
}

const char* bro_MistralTokenizer_decode(void* /*self*/, void* /*ids*/) {
    return "";
}

const char* bro_MistralTokenizer_applyChatTemplate(void* /*self*/, void* /*messages*/, bool /*addGenerationPrompt*/) {
    return "";
}

// --- Interface bro.lm.GemmaTokenizer ---
void* bro_GemmaTokenizer_create(void) {
    return new BroGemmaTokenizerImpl();
}

void bro_GemmaTokenizer_destroy(void* self) {
    delete static_cast<BroGemmaTokenizerImpl*>(self);
}

int32_t bro_GemmaTokenizer_get_eosId(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->eosId : 1;
}

int32_t bro_GemmaTokenizer_get_bosId(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->bosId : 2;
}

int32_t bro_GemmaTokenizer_get_padId(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->padId : 0;
}

int32_t bro_GemmaTokenizer_get_unkId(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->unkId : 3;
}

int32_t bro_GemmaTokenizer_get_vocabCount(void* self) {
    auto* t = static_cast<BroGemmaTokenizerImpl*>(self);
    return t ? t->vocabCount : 256000;
}

void* bro_GemmaTokenizer_encode(void* /*self*/, const char* /*text*/, bool /*addBos*/) {
    return nullptr;
}

const char* bro_GemmaTokenizer_decode(void* /*self*/, void* /*ids*/) {
    return "";
}

// --- Interface bro.lm.LMModel ---
void* bro_LMModel_create(void) {
    return new BroLMModelImpl();
}

void bro_LMModel_destroy(void* self) {
    delete static_cast<BroLMModelImpl*>(self);
}

const char* bro_LMModel_get_family(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->family.c_str() : "qwen3";
}

int32_t bro_LMModel_get_vocabSize(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_LMModel_get_hiddenSize(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_LMModel_get_numLayers(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_LMModel_get_maxSeqLen(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_LMModel_get_cacheLen(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    return m ? m->cacheLen : 0;
}

void bro_LMModel_allocateCache(void* self, int32_t maxTokens) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    if (m) m->cacheLen = maxTokens;
}

void bro_LMModel_resetCache(void* self) {
    auto* m = static_cast<BroLMModelImpl*>(self);
    if (m) m->cacheLen = 0;
}

void* bro_LMModel_generate(void* /*self*/, void* /*promptIds*/, void* /*opts*/) {
    return nullptr;
}

void* bro_LMModel_generateStream(void* /*self*/, void* /*promptIds*/, void* /*opts*/, void* /*onToken*/) {
    return nullptr;
}

// --- Interface bro.lm.Qwen35Model ---
void* bro_Qwen35Model_create(void) {
    return new BroQwen35ModelImpl();
}

void bro_Qwen35Model_destroy(void* self) {
    delete static_cast<BroQwen35ModelImpl*>(self);
}

const char* bro_Qwen35Model_get_family(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->family.c_str() : "qwen35";
}

int32_t bro_Qwen35Model_get_vocabSize(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_Qwen35Model_get_hiddenSize(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_Qwen35Model_get_numLayers(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_Qwen35Model_get_maxSeqLen(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_Qwen35Model_get_eosId(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->eosId : 151645;
}

int32_t bro_Qwen35Model_get_imEndId(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->imEndId : 151645;
}

int32_t bro_Qwen35Model_get_endoftextId(void* self) {
    auto* m = static_cast<BroQwen35ModelImpl*>(self);
    return m ? m->endoftextId : 151643;
}

void* bro_Qwen35Model_encode(void* /*self*/, const char* /*text*/, bool /*addSpecial*/) {
    return nullptr;
}

const char* bro_Qwen35Model_decode(void* /*self*/, void* /*ids*/) {
    return "";
}

void* bro_Qwen35Model_generate(void* /*self*/, const char* /*prompt*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.lm.Qwen3VLModel ---
void* bro_Qwen3VLModel_create(void) {
    return new BroQwen3VLModelImpl();
}

void bro_Qwen3VLModel_destroy(void* self) {
    delete static_cast<BroQwen3VLModelImpl*>(self);
}

const char* bro_Qwen3VLModel_get_family(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->family.c_str() : "qwen3vl";
}

int32_t bro_Qwen3VLModel_get_vocabSize(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->vocabSize : 151936;
}

int32_t bro_Qwen3VLModel_get_hiddenSize(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->hiddenSize : 4096;
}

int32_t bro_Qwen3VLModel_get_numLayers(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->numLayers : 32;
}

int32_t bro_Qwen3VLModel_get_maxSeqLen(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->maxSeqLen : 4096;
}

int32_t bro_Qwen3VLModel_get_eosId(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->eosId : 151645;
}

int32_t bro_Qwen3VLModel_get_imEndId(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->imEndId : 151645;
}

int32_t bro_Qwen3VLModel_get_endoftextId(void* self) {
    auto* m = static_cast<BroQwen3VLModelImpl*>(self);
    return m ? m->endoftextId : 151643;
}

void* bro_Qwen3VLModel_encode(void* /*self*/, const char* /*text*/, bool /*addSpecial*/) {
    return nullptr;
}

const char* bro_Qwen3VLModel_decode(void* /*self*/, void* /*ids*/) {
    return "";
}

void* bro_Qwen3VLModel_generate(void* /*self*/, const char* /*prompt*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.lm.NllbModel ---
void* bro_NllbModel_create(void) {
    return new BroNllbModelImpl();
}

void bro_NllbModel_destroy(void* self) {
    delete static_cast<BroNllbModelImpl*>(self);
}

const char* bro_NllbModel_get_family(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->family.c_str() : "nllb";
}

int32_t bro_NllbModel_get_vocabSize(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->vocabSize : 256206;
}

int32_t bro_NllbModel_get_dModel(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->dModel : 1024;
}

int32_t bro_NllbModel_get_encoderLayers(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->encoderLayers : 12;
}

int32_t bro_NllbModel_get_decoderLayers(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->decoderLayers : 12;
}

int32_t bro_NllbModel_get_languageCount(void* self) {
    auto* m = static_cast<BroNllbModelImpl*>(self);
    return m ? m->languageCount : 200;
}

bool bro_NllbModel_hasLanguage(void* /*self*/, const char* code) {
    return code && code[0] != '\0';
}

void* bro_NllbModel_translate(void* /*self*/, const char* /*text*/, const char* /*srcLang*/, const char* /*tgtLang*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.lm.ClipModel ---
void* bro_ClipModel_create(void) {
    return new BroClipModelImpl();
}

void bro_ClipModel_destroy(void* self) {
    delete static_cast<BroClipModelImpl*>(self);
}

int32_t bro_ClipModel_get_projectionDim(void* self) {
    auto* m = static_cast<BroClipModelImpl*>(self);
    return m ? m->projectionDim : 768;
}

void* bro_ClipModel_encodeText(void* /*self*/, void* /*text*/) {
    return nullptr;
}

void* bro_ClipModel_encodeImage(void* /*self*/, void* /*image*/) {
    return nullptr;
}

void* bro_ClipModel_score(void* /*self*/, void* /*text*/, void* /*image*/) {
    return nullptr;
}

// --- Interface bro.lm.T5Model ---
void* bro_T5Model_create(void) {
    return new BroT5ModelImpl();
}

void bro_T5Model_destroy(void* self) {
    delete static_cast<BroT5ModelImpl*>(self);
}

int32_t bro_T5Model_get_dModel(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->dModel : 4096;
}

int32_t bro_T5Model_get_maxLength(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->maxLength : 512;
}

int32_t bro_T5Model_get_padId(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->padId : 0;
}

int32_t bro_T5Model_get_eosId(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->eosId : 1;
}

int32_t bro_T5Model_get_vocabCount(void* self) {
    auto* m = static_cast<BroT5ModelImpl*>(self);
    return m ? m->vocabCount : 32128;
}

void* bro_T5Model_encode(void* /*self*/, const char* /*text*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.lm ---
void bro_lm_init(void) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->init) b->init();
}

void* bro_lm_loadQwen(const char* ggufPath, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadQwen) return b->loadQwen(ggufPath, opts);
    return nullptr;
}

void* bro_lm_loadMistral(const char* ggufPath, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadMistral) return b->loadMistral(ggufPath, opts);
    return nullptr;
}

void* bro_lm_loadGemma2(const char* modelDir, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadGemma2) return b->loadGemma2(modelDir, opts);
    return nullptr;
}

void* bro_lm_loadQwen35(const char* checkpointDir, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadQwen35) return b->loadQwen35(checkpointDir, opts);
    return new BroQwen35ModelImpl();
}

void* bro_lm_loadQwen3VL(const char* checkpointDir, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadQwen3VL) return b->loadQwen3VL(checkpointDir, opts);
    return new BroQwen3VLModelImpl();
}

void* bro_lm_loadNllb(const char* checkpointDir, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadNllb) return b->loadNllb(checkpointDir, opts);
    return new BroNllbModelImpl();
}

void* bro_lm_loadTokenizer(void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadTokenizer) return b->loadTokenizer(opts);
    return new BroQwenTokenizerImpl();
}

void* bro_lm_loadClip(void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadClip) return b->loadClip(opts);
    return new BroClipModelImpl();
}

void* bro_lm_loadT5(void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->loadT5) return b->loadT5(opts);
    return new BroT5ModelImpl();
}

void* bro_lm_generate(void* model, void* prompt, void* opts) {
    const auto* b = bro_get_lm_bridge();
    if (b && b->generate) return b->generate(model, prompt, opts);
    return new BroAsyncHandleImpl();
}

}
