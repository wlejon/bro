#pragma once
// The shaders a land node's tiles are made with (a ModuleShaders set): the
// fill with its world's land module in the place of
// shaders/ground/land_module.glsl, the module of no land it is built with
// (shaders/ground/scene_land_layer.glsl says what a module defines), the
// canopy of the plants standing on it in the place of
// shaders/ground/land_canopy.glsl (world/plant_canopy.h), and its
// processes' grid layout in the place of shaders/ground/land_grid_config.glsl
// (world/land_grid.h). The fill is the only shader that calls either: the
// rings (mesh_land.vert) read its tiles, the same for every world.

#include "scene/vulkan/frame/scene_module_shaders.h"

#include <string>

namespace bro::scene::vk {

class LandShaders : public ModuleShaders {
public:
    enum Stage : int {
        Fill,   // land_fill.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {BuiltinSceneShader::LandFillComp};

    /// The set with `module` as land_module.glsl, `canopy` (or none, "")
    /// as land_canopy.glsl and `grid` (or none, "") as land_grid_config.glsl,
    /// for a round body's land when `round` (land_body.glsl), compiled off
    /// the frame thread (ModuleShaders::poll).
    void start(const std::string& module, const std::string& canopy, const std::string& grid,
               const std::string& regions, bool round) {
        Overrides overrides{{"ground/land_module.glsl", module}};
        if (!canopy.empty()) overrides.push_back({"ground/land_canopy.glsl", canopy});
        if (!grid.empty()) overrides.push_back({"ground/land_grid_config.glsl", grid});
        if (!regions.empty()) overrides.push_back({"ground/land_region_config.glsl", regions});
        if (round) overrides.push_back({"ground/land_body.glsl", "#define LAND_ROUND\n"});
        ModuleShaders::start(kBuiltin, overrides);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

/// The shaders a land's processes run with (scene_land_process.h): the
/// runner with the world's processes in the place of
/// shaders/ground/land_process_module.glsl, the switch over its passes in
/// the place of land_process_dispatch.glsl and its grid's layout in the
/// place of land_grid_config.glsl.
class LandProcessShaders : public ModuleShaders {
public:
    enum Stage : int {
        Run,   // land_process.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {BuiltinSceneShader::LandProcessComp};

    void start(const std::string& module, const std::string& dispatch, const std::string& grid) {
        const Overrides overrides{{"ground/land_process_module.glsl", module},
                                  {"ground/land_process_dispatch.glsl", dispatch},
                                  {"ground/land_grid_config.glsl", grid}};
        ModuleShaders::start(kBuiltin, overrides);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

/// The shaders a land's regional grids run with (scene_land_regions.h): the
/// region runner with the world's processes in the place of
/// land_process_module.glsl, its regions' init and the switch over their
/// passes in the place of land_region_dispatch.glsl, and the grid's and the
/// regions' layouts.
class LandRegionShaders : public ModuleShaders {
public:
    enum Stage : int {
        Run,   // land_region.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {BuiltinSceneShader::LandRegionComp};

    void start(const std::string& module, const std::string& dispatch, const std::string& grid,
               const std::string& regions) {
        const Overrides overrides{{"ground/land_process_module.glsl", module},
                                  {"ground/land_region_dispatch.glsl", dispatch},
                                  {"ground/land_grid_config.glsl", grid},
                                  {"ground/land_region_config.glsl", regions}};
        ModuleShaders::start(kBuiltin, overrides);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
};

}  // namespace bro::scene::vk
