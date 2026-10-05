#include "webgl/vulkan/webgl_vk_spirv.h"

#include <cstring>

namespace bro::webgl::vk::spirv {

namespace {

constexpr size_t kHeaderWords = 5;
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kDecorationRelaxedPrecision = 0;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kStorageClassInput = 1;

uint32_t opcode(uint32_t word) { return word & 0xFFFFu; }
uint32_t wordCount(uint32_t word) { return word >> 16; }

// A literal string operand: UTF-8, NUL-terminated, packed into words.
std::string literalString(const uint32_t* words, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        char bytes[4];
        std::memcpy(bytes, &words[i], 4);
        for (char c : bytes) {
            if (c == '\0') return out;
            out.push_back(c);
        }
    }
    return out;
}

// Calls fn(offset, opcode, wordCount) for each instruction after the header;
// stops (returning false) at a malformed one.
template <typename Fn>
bool forEachInstruction(const std::vector<uint32_t>& words, Fn&& fn) {
    size_t at = kHeaderWords;
    while (at < words.size()) {
        const uint32_t n = wordCount(words[at]);
        if (n == 0 || at + n > words.size()) return false;
        fn(at, opcode(words[at]), n);
        at += n;
    }
    return true;
}

} // namespace

void stripRelaxedPrecision(std::vector<uint32_t>& words) {
    if (words.size() < kHeaderWords) return;
    std::vector<uint32_t> out(words.begin(), words.begin() + kHeaderWords);
    out.reserve(words.size());
    const bool ok = forEachInstruction(words, [&](size_t at, uint32_t op, uint32_t n) {
        const bool relaxed = (op == kOpDecorate && n >= 3 && words[at + 2] == kDecorationRelaxedPrecision) ||
                             (op == kOpMemberDecorate && n >= 4 && words[at + 3] == kDecorationRelaxedPrecision);
        if (!relaxed) out.insert(out.end(), words.begin() + at, words.begin() + at + n);
    });
    if (ok) words = std::move(out);
}

bool setInputLocations(std::vector<uint32_t>& words,
                       const std::unordered_map<std::string, uint32_t>& locations) {
    if (locations.empty()) return true;
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, bool> inputs;
    forEachInstruction(words, [&](size_t at, uint32_t op, uint32_t n) {
        if (op == kOpName && n >= 3) names[words[at + 1]] = literalString(&words[at + 2], n - 2);
        else if (op == kOpVariable && n >= 4 && words[at + 3] == kStorageClassInput) inputs[words[at + 2]] = true;
    });
    size_t patched = 0;
    forEachInstruction(words, [&](size_t at, uint32_t op, uint32_t n) {
        if (op != kOpDecorate || n < 4 || words[at + 2] != kDecorationLocation) return;
        const uint32_t id = words[at + 1];
        if (!inputs.count(id)) return;
        auto name = names.find(id);
        if (name == names.end()) return;
        auto loc = locations.find(name->second);
        if (loc == locations.end()) return;
        words[at + 3] = loc->second;
        ++patched;
    });
    return patched == locations.size();
}

} // namespace bro::webgl::vk::spirv
