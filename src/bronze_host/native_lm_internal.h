#pragma once

#include "native_lm_decl.h"
#include "host_natives.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::bronze_host {

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

struct T5EncodeSlot {
    std::vector<float> data;
    int32_t length = 0;
    int32_t dim = 0;
    std::vector<int32_t> ids;
};

struct LMModelPairSlot {
    void* model = nullptr;
    void* tokenizer = nullptr;
};

}  // namespace bro::bronze_host
