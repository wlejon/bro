#include "render/spirv_stats.h"

#include <unordered_map>

namespace bro {

namespace {

std::string decodeSpirvLiteralString(const uint32_t* words, size_t count) {
    std::string out;
    out.reserve(count * 4);
    for (size_t w = 0; w < count; ++w) {
        const uint32_t v = words[w];
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((v >> (b * 8)) & 0xFFu);
            if (c == '\0') return out;
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

SpirvStaticStats analyzeSpirvStaticStats(const uint32_t* words, size_t wordCount) {
    SpirvStaticStats out;
    out.spirvBytes = static_cast<uint32_t>(wordCount * sizeof(uint32_t));
    if (!words || wordCount < 5 || words[0] != 0x07230203u) return out;

    std::unordered_map<uint32_t, std::string> names;
    SpirvFunctionStats* curFn = nullptr;

    size_t i = 5;
    while (i < wordCount) {
        const uint32_t word0 = words[i];
        const uint16_t wc = static_cast<uint16_t>(word0 >> 16);
        const uint16_t op = static_cast<uint16_t>(word0 & 0xFFFFu);
        if (wc == 0 || i + wc > wordCount) break;

        ++out.instructions;
        if (curFn) ++curFn->instructions;

        if (op == 5 && wc >= 3) { // OpName
            const uint32_t targetId = words[i + 1];
            names[targetId] = decodeSpirvLiteralString(words + i + 2, wc - 2);
        } else if (op == 54 && wc >= 5) { // OpFunction
            ++out.functions;
            const uint32_t fnId = words[i + 2];
            out.functionBreakdown.push_back({});
            curFn = &out.functionBreakdown.back();
            curFn->id = fnId;
            auto it = names.find(fnId);
            curFn->name = (it != names.end() && !it->second.empty())
                              ? it->second
                              : ("fn_" + std::to_string(fnId));
            curFn->instructions = 1;
        } else if (op == 56) { // OpFunctionEnd
            curFn = nullptr;
        } else if (op == 61 || op == 227) { // OpLoad, OpAtomicLoad
            ++out.loads;
            if (curFn) ++curFn->loads;
        } else if (op == 62 || op == 228 || op == 99) { // OpStore, OpAtomicStore, OpImageWrite
            ++out.stores;
            if (curFn) ++curFn->stores;
        } else if (op >= 87 && op <= 98) { // OpImageSample*, OpImageFetch, OpImageGather, OpImageRead
            ++out.samples;
            if (curFn) ++curFn->samples;
        } else if ((op >= 126 && op <= 175) || (op >= 179 && op <= 209) || op == 12) {
            ++out.arithmetic;
            if (curFn) ++curFn->arithmetic;
        } else if (op == 246) { // OpLoopMerge
            ++out.loops;
            if (curFn) ++curFn->loops;
        } else if (op >= 249 && op <= 251) { // OpBranch, OpBranchConditional, OpSwitch
            ++out.branches;
            if (curFn) ++curFn->branches;
        }

        i += wc;
    }

    return out;
}

} // namespace bro
