#include "bronze_host/host_callee_namer.h"

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/heap.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bro::bronze_host {
namespace {

struct NamerState {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::string> bitsToName;
    std::unordered_map<const void*, std::string> ptrToName;
};

NamerState& getNamerState() {
    static auto* state = new NamerState();
    return *state;
}

}  // namespace

void registerHostCalleeName(bronze::Value fnVal, std::string_view name) {
    if (name.empty()) return;
    auto& s = getNamerState();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.bitsToName[fnVal.rawBits()] = std::string(name);

    if (fnVal.isObject()) {
        auto* hdr = fnVal.asObject<bronze::HeapObjectHeader>();
        if (hdr && hdr->flags == bronze::HeapKind::Function) {
            auto* fn = reinterpret_cast<bronze::FunctionHeader*>(hdr);
            if (fn->env_record.isPointer() || fn->env_record.isObject()) {
                s.bitsToName[fn->env_record.rawBits()] = std::string(name);
                void* handle = bronze::embed::handleData(fn->env_record);
                if (handle) {
                    s.ptrToName[handle] = std::string(name);
                }
            }
        }
    }
}

bool hostCalleeNamer(uint64_t calleeBits, void* code, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    auto& s = getNamerState();
    std::lock_guard<std::mutex> lock(s.mutex);

    // 1. Direct calleeBits match
    auto it = s.bitsToName.find(calleeBits);
    if (it != s.bitsToName.end()) {
        std::strncpy(out, it->second.c_str(), outSize);
        out[outSize - 1] = '\0';
        return true;
    }

    // 2. Inspect FunctionHeader env_record and handleData
    bronze::Value v(calleeBits);
    if (v.isObject()) {
        auto* hdr = v.asObject<bronze::HeapObjectHeader>();
        if (hdr && hdr->flags == bronze::HeapKind::Function) {
            auto* fn = reinterpret_cast<bronze::FunctionHeader*>(hdr);
            auto envIt = s.bitsToName.find(fn->env_record.rawBits());
            if (envIt != s.bitsToName.end()) {
                std::strncpy(out, envIt->second.c_str(), outSize);
                out[outSize - 1] = '\0';
                return true;
            }
            void* handle = bronze::embed::handleData(fn->env_record);
            if (handle) {
                auto ptrIt = s.ptrToName.find(handle);
                if (ptrIt != s.ptrToName.end()) {
                    std::strncpy(out, ptrIt->second.c_str(), outSize);
                    out[outSize - 1] = '\0';
                    return true;
                }
            }
        }
    }

    // 3. Fallback to code pointer if registered
    if (code) {
        auto codeIt = s.ptrToName.find(code);
        if (codeIt != s.ptrToName.end()) {
            std::strncpy(out, codeIt->second.c_str(), outSize);
            out[outSize - 1] = '\0';
            return true;
        }
    }

    return false;
}

void initHostCalleeNamer() {
    static bool s_initialized = false;
    if (s_initialized) return;
    s_initialized = true;
    bronze::embed::setProfileCalleeNamer(hostCalleeNamer);
}

}  // namespace bro::bronze_host
