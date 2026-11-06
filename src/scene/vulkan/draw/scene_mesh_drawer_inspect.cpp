#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include "render/spirv_stats.h"
#include "render/vulkan_pipeline_cache.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace bro::scene::vk {

namespace {

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void fillStageStats(const std::vector<uint32_t>& spirv,
                    const std::vector<StageFunctionCost>& costs,
                    VkShaderStageFlagBits stageBit,
                    const std::vector<render::PipelineExecutableStats>& execs,
                    ShaderStageStats& dst) {
    for (const auto& c : costs) dst.functionCosts.push_back({c.name, c.self, c.total});
    if (!spirv.empty()) {
        const bro::SpirvStaticStats s = bro::analyzeSpirvStaticStats(spirv);
        dst.present = true;
        dst.spirvBytes = s.spirvBytes;
        dst.instructions = s.instructions;
        dst.arithmetic = s.arithmetic;
        dst.samples = s.samples;
        dst.loads = s.loads;
        dst.stores = s.stores;
        dst.branches = s.branches;
        dst.loops = s.loops;
    }
    for (const auto& ex : execs) {
        if ((ex.stages & stageBit) == 0) continue;
        dst.present = true;
        for (const auto& st : ex.statistics) {
            ShaderStageExecStat item;
            item.name = st.name;
            item.description = st.description;
            item.isBool = st.isBool;
            if (st.isBool) {
                item.uval = st.value != 0.0 ? 1 : 0;
            } else {
                item.isFloat = true;
                item.fval = st.value;
                item.uval = st.value > 0.0 ? static_cast<uint64_t>(st.value) : 0;
                item.ival = static_cast<int64_t>(st.value);
            }
            dst.executables.push_back(std::move(item));

            const std::string low = toLowerAscii(st.name);
            const uint32_t v = st.value > 0.0 ? static_cast<uint32_t>(st.value) : 0u;
            if (low.find("sgpr") != std::string::npos) {
                dst.sgprs = std::max(dst.sgprs, v);
            } else if (low.find("vgpr") != std::string::npos || low.find("register") != std::string::npos ||
                       low.find("temp") != std::string::npos) {
                dst.vgprs = std::max(dst.vgprs, v);
            } else if (low.find("scratch") != std::string::npos || low.find("spill") != std::string::npos) {
                dst.scratchBytes = std::max(dst.scratchBytes, v);
            }
        }
    }
}

SceneRenderer::CustomShaderTarget shaderTargetForKind(MeshKind kind) {
    switch (kind) {
    case MeshKind::Instanced:
    case MeshKind::InstancedVegetation: return SceneRenderer::CustomShaderTarget::Instanced;
    case MeshKind::Skinned: return SceneRenderer::CustomShaderTarget::Skinned;
    default: return SceneRenderer::CustomShaderTarget::Static;
    }
}

} // namespace

void SceneMeshDrawer::beginProbe(int x, int y) {
    probeActive_ = true;
    probeX_ = x;
    probeY_ = y;
    probeRecords_.clear();
}

std::vector<SceneNodeProbeResult> SceneMeshDrawer::endProbe(const SceneGraph& graph) {
    probeActive_ = false;
    std::vector<SceneNodeProbeResult> results;
    for (const ProbeRecord& rec : probeRecords_) {
        if (!rec.mapped || rec.slots.empty()) continue;
        SceneNodeProbeResult nodeRes;
        nodeRes.nodeId = rec.nodeId;
        if (const SceneNode* n = graph.findById(rec.nodeId)) {
            nodeRes.nodeName = n->name();
        }
        if (nodeRes.nodeName.empty()) {
            nodeRes.nodeName = "node_" + std::to_string(rec.nodeId);
        }
        for (const KernelProbeSlot& slot : rec.slots) {
            const size_t base = 2 + static_cast<size_t>(slot.index) * 5;
            if (rec.mapped[base] == 0.0f) continue;
            SceneProbeValue pv;
            pv.name = slot.name;
            pv.components = std::clamp(static_cast<int>(slot.components), 1, 4);
            for (int c = 0; c < pv.components; ++c) {
                pv.values[c] = rec.mapped[base + 1 + c];
            }
            nodeRes.values.push_back(std::move(pv));
        }
        if (!nodeRes.values.empty()) {
            results.push_back(std::move(nodeRes));
        }
    }
    probeRecords_.clear();
    return results;
}

bool SceneMeshDrawer::shaderStats(const CustomShaderState& cs, MeshKind kind, bool isVolume, NodeShaderStats& out) {
    out = NodeShaderStats{};
    if (device_ == VK_NULL_HANDLE) {
        std::string err;
        if (isVolume) {
            KernelVolumeHook hook;
            if (!compileKernelVolumeHook(cs.module, cs.moduleFile, cs.modules, hook, err, cs.knobs)) return false;
            fillStageStats(hook.main.spirv, hook.main.costs, VK_SHADER_STAGE_FRAGMENT_BIT, {}, out.fragment);
            if (hook.medium) fillStageStats(hook.nearPass.spirv, hook.nearPass.costs, VK_SHADER_STAGE_FRAGMENT_BIT, {}, out.nearStage);
            return true;
        }
        KernelMeshHook hook;
        if (!compileKernelMeshHook(shaderTargetForKind(kind), cs.module, cs.moduleFile, cs.modules, hook, err, cs.knobs))
            return false;
        if (hook.hasFragment) fillStageStats(hook.fs.spirv, hook.fs.costs, VK_SHADER_STAGE_FRAGMENT_BIT, {}, out.fragment);
        if (hook.hasVertex) fillStageStats(hook.vs.spirv, hook.vs.costs, VK_SHADER_STAGE_VERTEX_BIT, {}, out.vertex);
        return true;
    }

    CustomProgram* prog = isVolume ? volumeProgram(cs) : program(cs, kind);
    if (!prog) return false;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (isVolume) {
        TargetFormat target = TargetFormat::colorOnly(SceneTargets::kHdrFormat);
        target.color[1] = VK_FORMAT_R32_SFLOAT;
        target.colorCount = 2;
        pipeline = prog->pipelines.get(0u, target, [&] {
            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendAttachmentState overwrite{};
            overwrite.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
            SceneVkPipelineBuilder b;
            b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsVolume_, "volume_vert")
             .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, prog->fs, prog->fsEntry.c_str())
             .setVertexInput({}, {})
             .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
             .setPolygonMode(VK_POLYGON_MODE_FILL)
             .setCullMode(VK_CULL_MODE_NONE)
             .setTarget(target)
             .setColorBlendAttachment(0, blend)
             .setColorBlendAttachment(1, overwrite)
             .setLabel("volume")
             .disableDepthTest();
            return b.build(device_, layout_);
        }, /*async=*/false);
    } else {
        TargetFormat target;
        target.color[0] = SceneTargets::kHdrFormat;
        target.colorCount = 1;
        target.depth = SceneTargets::kDepthFormat;
        target.samples = VK_SAMPLE_COUNT_1_BIT;
        const auto st = shaderTargetForKind(kind);
        pipeline = prog->pipelines.get(0u, target, [&] {
            VkShaderModule vs = prog->vs ? prog->vs : vsPassthrough_[static_cast<int>(st)];
            VkShaderModule fs = prog->fs ? prog->fs : fs_[0][0][0];
            const char* fsEntry = prog->fs ? prog->fsEntry.c_str() : "mesh_frag";
            return buildPipeline(vs, fs, kind, 0u, target, "custom mesh", true, prog->vsEntry.c_str(), fsEntry);
        }, /*async=*/false);
    }

    const std::vector<render::PipelineExecutableStats> execs =
        pipeline ? render::queryPipelineExecutableStats(device_, pipeline)
                 : std::vector<render::PipelineExecutableStats>{};

    fillStageStats(prog->fsSpirv, prog->fsCosts, VK_SHADER_STAGE_FRAGMENT_BIT, execs, out.fragment);
    fillStageStats(prog->vsSpirv, prog->vsCosts, VK_SHADER_STAGE_VERTEX_BIT, execs, out.vertex);

    if (isVolume && prog->medium) {
        VkPipeline nearPipe = prog->nearPipelines.get(0u, TargetFormat{}, [&] {
            SceneVkPipelineBuilder b;
            b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsVolume_, "volume_vert")
             .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, prog->fsNear, prog->fsNearEntry.c_str())
             .setVertexInput({}, {})
             .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
             .setPolygonMode(VK_POLYGON_MODE_FILL)
             .setCullMode(VK_CULL_MODE_NONE)
             .setTarget(TargetFormat{})
             .setLabel("volume near")
             .disableDepthTest();
            return b.build(device_, nearLayout_);
        }, /*async=*/false);
        const std::vector<render::PipelineExecutableStats> nearExecs =
            nearPipe ? render::queryPipelineExecutableStats(device_, nearPipe)
                     : std::vector<render::PipelineExecutableStats>{};
        fillStageStats(prog->nearSpirv, prog->nearCosts, VK_SHADER_STAGE_FRAGMENT_BIT, nearExecs, out.nearStage);
    }
    return true;
}

} // namespace bro::scene::vk
