#include "render/kernel_shader.h"

#include "util/log.h"
#include "util/user_dirs.h"

#include <brokernel/brokernel.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

#include "present.bk.src.h"
#include "frame_diff.bk.src.h"
#include "kernel_canvas.bk.src.h"

namespace bro::render {

KernelShader renderKernelShader(RenderKernelSource source, const char* entry, std::string* err) {
    static std::mutex s_mu;
    static std::map<std::pair<int, std::string>, KernelShader> s_cache;
    const auto key = std::make_pair(static_cast<int>(source), std::string(entry));
    std::lock_guard<std::mutex> lock(s_mu);
    if (auto it = s_cache.find(key); it != s_cache.end()) return it->second;

    const char* file = "present.bk";
    const char* text = kBkPresentSrc;
    if (source == RenderKernelSource::FrameDiff) {
        file = "frame_diff.bk";
        text = kBkFrameDiffSrc;
    } else if (source == RenderKernelSource::KernelCanvas) {
        file = "kernel_canvas.bk";
        text = kBkKernelCanvasSrc;
    }
    brokernel::Compiler compiler;
    brokernel::CompileOptions opts;
    opts.entry_name = entry;
    opts.compile_cpu = false;
    const brokernel::CompileResult res = compiler.compile_source(text, file, opts);
    KernelShader out;
    const brokernel::CompiledKernel* k = res.ok ? res.find_kernel(entry) : nullptr;
    if (!k || k->spirv.words.empty()) {
        if (err) *err = res.ok ? std::string("no SPIR-V for ") + file + ":" + entry : res.error_message();
        return out;
    }
    out.spirv = k->spirv.words;
    out.entry = k->spirv.entry;
    s_cache.emplace(key, out);
    return out;
}

void configureKernelCache() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        const char* off = std::getenv("BRO_KERNEL_CACHE");
        if (off && (std::strcmp(off, "0") == 0 || std::strcmp(off, "false") == 0)) {
            LOG_INFO("kernel cache: off (BRO_KERNEL_CACHE=%s)", off);
            return;
        }
        brokernel::CompileCacheConfig cfg;
        const char* dir = std::getenv("BRO_KERNEL_CACHE_DIR");
        cfg.dir = std::filesystem::u8path(dir && *dir ? std::string(dir) : util::userCacheDir() + "/kernel-cache");
        cfg.max_bytes = uint64_t{512} << 20;
        brokernel::set_compile_cache(std::move(cfg));
    });
}

void flushKernelCache() { brokernel::flush_compile_cache(); }

} // namespace bro::render
