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
constexpr uint32_t kStorageClassPrivate = 6;
constexpr uint32_t kStorageClassFunction = 7;
constexpr uint32_t kOpTypeBool = 20;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeMatrix = 24;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpConstantNull = 46;

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

void zeroInitializeVariables(std::vector<uint32_t>& words) {
    if (words.size() < kHeaderWords) return;
    // Which types may have a null constant (scalars and composites of them),
    // where each is declared, and what each pointer type points to.
    std::unordered_map<uint32_t, bool> nullable;
    std::unordered_map<uint32_t, size_t> declEnd;
    std::unordered_map<uint32_t, uint32_t> pointee;
    std::unordered_map<uint32_t, uint32_t> nullOf;  // type -> its OpConstantNull, once made
    std::vector<size_t> uninit;                     // offsets of OpVariables to give one
    forEachInstruction(words, [&](size_t at, uint32_t op, uint32_t n) {
        if (n < 2) return;
        const uint32_t id = words[at + 1];
        switch (op) {
            case kOpTypeBool: case kOpTypeInt: case kOpTypeFloat:
                nullable[id] = true; declEnd[id] = at + n; break;
            case kOpTypeVector: case kOpTypeMatrix: case kOpTypeArray:
                if (n >= 3) { nullable[id] = nullable[words[at + 2]]; declEnd[id] = at + n; }
                break;
            case kOpTypeStruct: {
                bool all = true;
                for (uint32_t k = 2; k < n; ++k) all = all && nullable[words[at + k]];
                nullable[id] = all;
                declEnd[id] = at + n;
                break;
            }
            case kOpTypePointer:
                if (n >= 4) pointee[id] = words[at + 3];
                break;
            case kOpConstantNull:
                if (n >= 3) nullOf[words[at + 1]] = words[at + 2];
                break;
            case kOpVariable:
                if (n == 4 && (words[at + 3] == kStorageClassPrivate || words[at + 3] == kStorageClassFunction))
                    uninit.push_back(at);
                break;
            default: break;
        }
    });
    if (uninit.empty()) return;
    // New OpConstantNulls go right after their type's declaration; each
    // variable then gains its initializer operand. Edits are collected as
    // insertions keyed by offset and applied in one rebuild.
    uint32_t bound = words[3];
    std::unordered_map<size_t, std::vector<uint32_t>> insertAfter;  // offset -> words to insert there
    std::unordered_map<size_t, uint32_t> initFor;                    // OpVariable offset -> initializer id
    for (size_t at : uninit) {
        auto pt = pointee.find(words[at + 1]);
        if (pt == pointee.end() || !nullable[pt->second]) continue;
        const uint32_t type = pt->second;
        auto have = nullOf.find(type);
        uint32_t nullId;
        if (have != nullOf.end()) {
            nullId = have->second;
        } else {
            nullId = bound++;
            nullOf[type] = nullId;
            auto& ins = insertAfter[declEnd[type]];
            ins.insert(ins.end(), {(3u << 16) | kOpConstantNull, type, nullId});
        }
        initFor[at] = nullId;
    }
    if (initFor.empty()) return;
    std::vector<uint32_t> out(words.begin(), words.begin() + kHeaderWords);
    out.reserve(words.size() + initFor.size() * 4);
    out[3] = bound;
    const bool ok = forEachInstruction(words, [&](size_t at, uint32_t, uint32_t n) {
        if (auto it = insertAfter.find(at); it != insertAfter.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        auto init = initFor.find(at);
        if (init == initFor.end()) {
            out.insert(out.end(), words.begin() + at, words.begin() + at + n);
        } else {
            out.push_back((5u << 16) | kOpVariable);
            out.insert(out.end(), words.begin() + at + 1, words.begin() + at + 4);
            out.push_back(init->second);
        }
    });
    if (!ok) return;
    if (auto it = insertAfter.find(words.size()); it != insertAfter.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    words = std::move(out);
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
