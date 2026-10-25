#include "scene/vulkan/frame/scene_module_shaders.h"

#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"

#include <chrono>

namespace bro::scene::vk {

bool ModuleShaders::createBuiltin(VkDevice device, std::span<const BuiltinSceneShader> stages) {
    modules_.assign(stages.size(), VK_NULL_HANDLE);
    for (size_t s = 0; s < stages.size(); ++s) {
        modules_[s] = SceneVkShaderCompiler::createBuiltinModule(device, stages[s]);
        if (!modules_[s]) {
            destroy(device);
            return false;
        }
    }
    state_ = State::Ready;
    return true;
}

void ModuleShaders::start(std::span<const BuiltinSceneShader> stages, const Overrides& overrides) {
    stageNames_.clear();
    for (BuiltinSceneShader s : stages) stageNames_.push_back(SceneVkShaderCompiler::builtinName(s));
    future_ = shader_warmup::request(stages, overrides);
    state_ = State::Pending;
}

ModuleShaders::State ModuleShaders::poll(VkDevice device, std::string& err) {
    if (state_ != State::Pending) return state_;
    if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (shader_warmup::async()) {
            pipeline_async::noteSkip();
            return State::Pending;
        }
        // (Headless: the frame holds everything it draws.)
        const double t0 = stalls::nowMs();
        future_.wait();
        stalls::noteShaderSet(stalls::nowMs() - t0);
    }
    const shader_warmup::SetResult& r = *future_.get();
    future_ = {};
    const size_t n = r.spirv.size();
    err.clear();
    for (size_t s = 0; s < n; ++s) {
        if (r.spirv[s].empty()) {
            // One shader's diagnostics: the module's errors repeat in each.
            err = stageNames_[s] + ": " + r.errs[s];
            state_ = State::Failed;
            return state_;
        }
    }
    modules_.assign(n, VK_NULL_HANDLE);
    for (size_t s = 0; s < n; ++s) {
        modules_[s] = SceneVkShaderModule::create(device, r.spirv[s]);
        if (!modules_[s]) {
            destroy(device);
            err = "could not create a shader module";
            state_ = State::Failed;
            return state_;
        }
    }
    state_ = State::Ready;
    return state_;
}

void ModuleShaders::destroy(VkDevice device) {
    for (VkShaderModule& m : modules_) {
        SceneVkShaderModule::destroy(device, m);
        m = VK_NULL_HANDLE;
    }
}

}  // namespace bro::scene::vk
