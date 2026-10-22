#include "scene/vulkan/ground/scene_cover_shaders.h"

#include "scene/vulkan/frame/scene_vk_pipeline.h"

#include <thread>
#include <utility>
#include <vector>

namespace bro::scene::vk {

bool CoverShaders::createBuiltin(VkDevice device) {
    for (int s = 0; s < kStages; ++s) {
        modules_[s] = SceneVkShaderCompiler::createBuiltinModule(device, kBuiltin[s]);
        if (!modules_[s]) {
            destroy(device);
            return false;
        }
    }
    return true;
}

bool CoverShaders::create(VkDevice device, const std::string& sward, const std::string& herbs, std::string& err) {
    std::vector<std::pair<std::string, std::string>> overrides;
    if (!sward.empty()) overrides.emplace_back("ground/sward_module.glsl", sward);
    if (!herbs.empty()) overrides.emplace_back("ground/herb_module.glsl", herbs);
    std::vector<uint32_t> spirv[kStages];
    std::string errs[kStages];
    {
        std::vector<std::thread> threads;
        threads.reserve(kStages);
        for (int s = 0; s < kStages; ++s)
            threads.emplace_back([&, s] { spirv[s] = SceneVkShaderCompiler::compileBuiltinWith(kBuiltin[s], overrides, &errs[s]); });
        for (std::thread& t : threads) t.join();
    }
    err.clear();
    for (int s = 0; s < kStages; ++s) {
        if (spirv[s].empty()) {
            // One shader's diagnostics: the module's errors repeat in each.
            err = std::string(SceneVkShaderCompiler::builtinName(kBuiltin[s])) + ": " + errs[s];
            return false;
        }
    }
    for (int s = 0; s < kStages; ++s) {
        modules_[s] = SceneVkShaderModule::create(device, spirv[s]);
        if (!modules_[s]) {
            destroy(device);
            err = "could not create a shader module";
            return false;
        }
    }
    return true;
}

void CoverShaders::destroy(VkDevice device) {
    for (VkShaderModule& m : modules_) {
        SceneVkShaderModule::destroy(device, m);
        m = VK_NULL_HANDLE;
    }
}

}  // namespace bro::scene::vk
