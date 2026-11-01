#include "render/kernel_shader.h"

#include <brokernel/brokernel.h>

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

} // namespace bro::render
