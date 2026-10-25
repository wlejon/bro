#include "scene/vulkan/frame/scene_module_shaders.h"

#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"

#include <thread>

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
    return true;
}

bool ModuleShaders::create(VkDevice device, std::span<const BuiltinSceneShader> stages, const Overrides& overrides,
                           std::string& err) {
    const size_t n = stages.size();
    std::vector<std::vector<uint32_t>> spirv(n);
    std::vector<std::string> errs(n);
    {
        const double t0 = stalls::nowMs();
        std::vector<std::thread> threads;
        threads.reserve(n);
        for (size_t s = 0; s < n; ++s)
            threads.emplace_back([&, s] { spirv[s] = SceneVkShaderCompiler::compileBuiltinWith(stages[s], overrides, &errs[s]); });
        for (std::thread& t : threads) t.join();
        stalls::noteShaderSet(stalls::nowMs() - t0);
    }
    err.clear();
    for (size_t s = 0; s < n; ++s) {
        if (spirv[s].empty()) {
            // One shader's diagnostics: the module's errors repeat in each.
            err = std::string(SceneVkShaderCompiler::builtinName(stages[s])) + ": " + errs[s];
            return false;
        }
    }
    modules_.assign(n, VK_NULL_HANDLE);
    for (size_t s = 0; s < n; ++s) {
        modules_[s] = SceneVkShaderModule::create(device, spirv[s]);
        if (!modules_[s]) {
            destroy(device);
            err = "could not create a shader module";
            return false;
        }
    }
    return true;
}

void ModuleShaders::destroy(VkDevice device) {
    for (VkShaderModule& m : modules_) {
        SceneVkShaderModule::destroy(device, m);
        m = VK_NULL_HANDLE;
    }
}

}  // namespace bro::scene::vk
