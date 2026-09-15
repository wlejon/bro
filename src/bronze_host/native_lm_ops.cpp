#include "native_lm_internal.h"

using namespace bro::bronze_host;

static thread_local LMModelPairSlot tl_loadQwenSlot;
static thread_local LMModelPairSlot tl_loadMistralSlot;
static thread_local LMModelPairSlot tl_loadGemma2Slot;

extern "C" {

#if BRO_WITH_LM

void bro_lm_init(void) {}

void bro_lm_loadQwen(const char* /*ggufPath*/, const char* /*opts_device*/,
                     bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                     int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    tl_loadQwenSlot.model = new BroLMModelImpl();
    tl_loadQwenSlot.tokenizer = new BroQwenTokenizerImpl();
}

void* bro_lm_loadQwen_model(void) {
    return tl_loadQwenSlot.model;
}

void* bro_lm_loadQwen_tokenizer(void) {
    return tl_loadQwenSlot.tokenizer;
}

void bro_lm_loadMistral(const char* /*ggufPath*/, const char* /*opts_device*/,
                        bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                        int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    tl_loadMistralSlot.model = new BroLMModelImpl();
    tl_loadMistralSlot.tokenizer = new BroMistralTokenizerImpl();
}

void* bro_lm_loadMistral_model(void) {
    return tl_loadMistralSlot.model;
}

void* bro_lm_loadMistral_tokenizer(void) {
    return tl_loadMistralSlot.tokenizer;
}

void bro_lm_loadGemma2(const char* /*modelDir*/, const char* /*opts_device*/,
                       bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                       int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    tl_loadGemma2Slot.model = new BroLMModelImpl();
    tl_loadGemma2Slot.tokenizer = new BroGemmaTokenizerImpl();
}

void* bro_lm_loadGemma2_model(void) {
    return tl_loadGemma2Slot.model;
}

void* bro_lm_loadGemma2_tokenizer(void) {
    return tl_loadGemma2Slot.tokenizer;
}

void* bro_lm_loadQwen35(const char* /*checkpointDir*/, const char* /*opts_device*/,
                        bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                        int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    return new BroQwen35ModelImpl();
}

void* bro_lm_loadQwen3VL(const char* /*checkpointDir*/, const char* /*opts_device*/,
                         bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                         int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    return new BroQwen3VLModelImpl();
}

void* bro_lm_loadNllb(const char* /*checkpointDir*/, const char* /*opts_device*/,
                      bool /*opts_tokenizerPath_given*/, const char* /*opts_tokenizerPath*/,
                      int32_t /*opts_maxSeqLen*/, uint64_t /*opts_onReady*/, uint64_t /*opts_onError*/) {
    return new BroNllbModelImpl();
}

void* bro_lm_loadTokenizer(const char* /*opts_vocabPath*/, const char* /*opts_mergesPath*/) {
    return new BroQwenTokenizerImpl();
}

void* bro_lm_loadClip(const char* /*opts_vocabPath*/, const char* /*opts_mergesPath*/,
                      bool /*opts_weightsPath_given*/, const char* /*opts_weightsPath*/,
                      bool /*opts_textPath_given*/, const char* /*opts_textPath*/,
                      bool /*opts_imagePath_given*/, const char* /*opts_imagePath*/,
                      bool /*opts_projectionPath_given*/, const char* /*opts_projectionPath*/,
                      const char* /*opts_textPrefix*/, const char* /*opts_visionPrefix*/,
                      const char* /*opts_projectionPrefix*/, const char* /*opts_device*/) {
    return new BroClipModelImpl();
}

void* bro_lm_loadT5(const char* /*opts_tokenizerPath*/, bool /*opts_ggufPath_given*/,
                    const char* /*opts_ggufPath*/, bool /*opts_weightsPath_given*/,
                    const char* /*opts_weightsPath*/, const char* /*opts_shards*/,
                    const char* /*opts_prefix*/, int32_t /*opts_maxLength*/,
                    bool /*opts_quantizeWeights*/, bool /*opts_config_vocabSize_given*/,
                    int32_t /*opts_config_vocabSize*/, bool /*opts_config_dModel_given*/,
                    int32_t /*opts_config_dModel*/, bool /*opts_config_dFf_given*/,
                    int32_t /*opts_config_dFf*/, bool /*opts_config_dKv_given*/,
                    int32_t /*opts_config_dKv*/, bool /*opts_config_numHeads_given*/,
                    int32_t /*opts_config_numHeads*/, bool /*opts_config_numLayers_given*/,
                    int32_t /*opts_config_numLayers*/, const char* /*opts_device*/) {
    return new BroT5ModelImpl();
}

#endif  // BRO_WITH_LM

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_lm(std::string* error);

bool registerLmNatives(std::string* error) {
    return registerNatives_lm(error);
}

}  // namespace bro::bronze_host
