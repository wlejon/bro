#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bro {

struct SpirvFunctionStats {
    uint32_t id = 0;
    std::string name;
    uint32_t instructions = 0;
    uint32_t arithmetic = 0;
    uint32_t samples = 0;
    uint32_t loads = 0;
    uint32_t stores = 0;
    uint32_t branches = 0;
    uint32_t loops = 0;
};

struct SpirvStaticStats {
    uint32_t spirvBytes = 0;
    uint32_t instructions = 0;
    uint32_t arithmetic = 0;
    uint32_t samples = 0;
    uint32_t loads = 0;
    uint32_t stores = 0;
    uint32_t branches = 0;
    uint32_t loops = 0;
    uint32_t functions = 0;
    std::vector<SpirvFunctionStats> functionBreakdown;
};

SpirvStaticStats analyzeSpirvStaticStats(const uint32_t* words, size_t wordCount);

inline SpirvStaticStats analyzeSpirvStaticStats(const std::vector<uint32_t>& words) {
    return analyzeSpirvStaticStats(words.data(), words.size());
}

} // namespace bro
