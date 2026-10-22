// The land (scene/nodes/land_node.h; docs/scene-world-architecture.md §13,
// "the land"): each land node's tiles, chosen for the camera
// (scene/world/surface_layers.h), offered to the world service (§6) as
// fills, made as it grants them by its module in a compute pass
// (land_fill.comp, the node's LandShaders set), and its rings drawn over
// them (mesh_land.vert reads them through the page table).

#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include "scene/nodes/grass_node.h"
#include "scene/nodes/land_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/ground/scene_land_layout.h"
#include "scene/vulkan/shaders/world/world_horizon_layout.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

// The tiles' layers: the heights (and the canopy's top), the surface
// (albedo, herbs), what covers it (LandSurface::cover; a grass node on the
// land reads these three, scene_mesh_drawer_grass.cpp) and the canopy of
// the plants standing on it (their leaves' albedo, their cover).
constexpr VkFormat kLandFormats[4] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                      VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
// A storage buffer's offset alignment no device exceeds.
constexpr VkDeviceSize kStorageAlign = 256;
// How far above and below the land's heights a tile's box reaches.
constexpr float kDepthMargin = 2.0f;

}  // namespace

bool SceneMeshDrawer::setupLand(SceneGpu& gpu) {
    // The fill's set: the layers (storage, whole arrays), the fills and the
    // module's data.
    SceneVkDescriptorLayoutBuilder fill;
    fill.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    fill.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    fill.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    fill.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    fill.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    fill.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    landFillSetLayout_ = fill.build(device_);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, LF_P_WORDS * 4};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &landFillSetLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (!landFillSetLayout_ || vkCreatePipelineLayout(device_, &info, nullptr, &landFillLayout_) != VK_SUCCESS) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land fill's layout");
        return false;
    }
    // The fills run as a compute dispatch inside the frame's graphics
    // stream: they are read by the turf's fills and the frame's draws.
    if (landKind_ < 0) landKind_ = gpu.world.addKind(this, {"land", WorldSchedule::Raster, 0.01f});
    if (landKind_ < 0) {
        LOG_ERROR("SceneMeshDrawer: the world service has no room for the land's fills");
        return false;
    }
    return setupLandHorizon(gpu) && setupLandSun(gpu);
}

bool SceneMeshDrawer::setupLandHorizon(SceneGpu& gpu) {
    // Its set: the LandDraw words, the tiles' heights, surface and page
    // table, and the horizon; the eye pushed.
    SceneVkDescriptorLayoutBuilder set;
    set.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    for (uint32_t b = 1; b <= 3; ++b)
        set.addBinding(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    horizonSetLayout_ = set.build(device_);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &horizonSetLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (!horizonSetLayout_ || vkCreatePipelineLayout(device_, &info, nullptr, &horizonLayout_) != VK_SUCCESS) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land horizon's layout");
        return false;
    }
    horizonModule_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::LandHorizonComp);
    horizonPipeline_ = horizonModule_ ? createComputePipeline(device_, horizonModule_, horizonLayout_, "land horizon")
                                      : VK_NULL_HANDLE;
    if (!horizonPipeline_) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land horizon's pipeline");
        return false;
    }
    // Recorded after the land's fills, which it reads.
    if (horizonKind_ < 0) horizonKind_ = gpu.world.addKind(this, {"land horizon", WorldSchedule::Raster, 0.03f});
    if (horizonKind_ < 0) {
        LOG_ERROR("SceneMeshDrawer: the world service has no room for the land's horizon");
        return false;
    }
    return true;
}

void SceneMeshDrawer::cleanupLandHorizon(SceneGpu& gpu) {
    horizon_.destroy(gpu.allocator);
    if (horizonPipeline_) vkDestroyPipeline(device_, horizonPipeline_, nullptr);
    SceneVkShaderModule::destroy(device_, horizonModule_);
    if (horizonLayout_) vkDestroyPipelineLayout(device_, horizonLayout_, nullptr);
    if (horizonSetLayout_) vkDestroyDescriptorSetLayout(device_, horizonSetLayout_, nullptr);
    horizonPipeline_ = VK_NULL_HANDLE;
    horizonModule_ = VK_NULL_HANDLE;
    horizonLayout_ = VK_NULL_HANDLE;
    horizonSetLayout_ = VK_NULL_HANDLE;
}

void SceneMeshDrawer::cleanupLand(SceneGpu& gpu) {
    cleanupLandHorizon(gpu);
    cleanupLandSun(gpu);
    for (auto& [id, lt] : lands_) lt->atlas.destroy(gpu.device, gpu.allocator);
    lands_.clear();
    for (VkPipeline p : landFillPipelines_)
        if (p) vkDestroyPipeline(device_, p, nullptr);
    landFillPipelines_.clear();
    for (auto& s : landSets_) s->destroy(device_);
    landSets_.clear();
    landByModule_.clear();
    if (landFillLayout_) vkDestroyPipelineLayout(device_, landFillLayout_, nullptr);
    if (landFillSetLayout_) vkDestroyDescriptorSetLayout(device_, landFillSetLayout_, nullptr);
    landFillLayout_ = VK_NULL_HANDLE;
    landFillSetLayout_ = VK_NULL_HANDLE;
}

const LandShaders* SceneMeshDrawer::landShaders(const LandNode& node) {
    if (!node.moduleSourceHash()) return nullptr;
    const uint64_t key =
        node.moduleSourceHash() ^ (node.canopySourceHash() * 0x9e3779b97f4a7c15ull) ^ (node.round() ? 1ull : 0ull);
    auto it = landByModule_.find(key);
    if (it == landByModule_.end()) {
        // Compiled at the first frame that draws it (through the SPIR-V
        // cache); one that does not compile is logged once and draws nothing.
        auto set = std::make_unique<LandShaders>();
        std::string err;
        int index = -1;
        if (set->create(device_, node.moduleSource(), node.canopySource(), node.round(), err)) {
            index = static_cast<int>(landSets_.size());
            set->index = static_cast<uint32_t>(index);
            landSets_.push_back(std::move(set));
        } else {
            LOG_ERROR("Land node %u: its module did not compile, so it draws no land:\n%s", node.id(), err.c_str());
        }
        it = landByModule_.emplace(key, index).first;
    }
    return it->second < 0 ? nullptr : landSets_[static_cast<size_t>(it->second)].get();
}

VkPipeline SceneMeshDrawer::landFillPipeline(const LandShaders& shaders) {
    if (shaders.index >= landFillPipelines_.size()) landFillPipelines_.resize(shaders.index + 1, VK_NULL_HANDLE);
    VkPipeline& p = landFillPipelines_[shaders.index];
    if (!p) p = createComputePipeline(device_, shaders[LandShaders::Fill], landFillLayout_, "land fill");
    return p;
}

SceneMeshDrawer::LandTiles* SceneMeshDrawer::landTiles(SceneFrame& frame, LandNode& node) {
    auto& slot = lands_[node.id()];
    // Its page table holds every level of each of its chart's roots: made
    // again when its body's roots change.
    const int roots = node.tileMap() ? node.tileMap()->roots() : 1;
    if (slot && slot->roots != roots) {
        slot->atlas.destroy(frame.gpu.device, frame.gpu.allocator);
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<LandTiles>();
        slot->roots = roots;
        const SurfaceLayerConfig config = node.tileConfig();
        slot->tiles.configure(config, bromath::aempty3(), node.tileMap());
        if (!slot->atlas.create(frame.gpu.device, frame.gpu.allocator, config, config.maxLevels * roots,
                                kLandFormats, SceneSurfaceLayerAtlas::Fill::Compute)) {
            LOG_ERROR("Land node %u: its tiles' atlas could not be made, so it draws no land", node.id());
            slot->failed = true;
        }
    }
    return slot->failed ? nullptr : slot.get();
}

void SceneMeshDrawer::landWords(const SceneFrame& frame, const LandNode& node, const LandTiles& lt, float* words) {
    const LandNode::Rings& r = node.rings();
    const SurfaceLayerConfig& config = lt.tiles.config();
    std::fill(words, words + LD_WORDS, 0.0f);
    // The rings centre on the eye's point on the land's map.
    const bromath::Vec3 eye = frame.world.eye;
    const Place* place = node.place();
    double ex = eye.x, ez = eye.z, eh = eye.y;
    if (place) {
        const double e[3] = {eye.x, eye.y, eye.z};
        place->landToMap(e, ex, ez, eh);
    }
    words[LD_LAND + 0] = static_cast<float>(node.mapReach());
    words[LD_LAND + 1] = static_cast<float>(ex);
    words[LD_LAND + 2] = static_cast<float>(ez);
    words[LD_EYE + 0] = eye.x;
    words[LD_EYE + 1] = eye.y;
    words[LD_EYE + 2] = eye.z;
    words[LD_BODY + 1] = static_cast<float>(lt.firstRing);
    if (node.round()) {
        // Each cube face's axes in the place frame (its basis' transpose
        // takes the body's to it) and where the anchor (the place's up) lies
        // on its chart, in level-0 tiles.
        const Body& body = place->body();
        const double* B = place->basis();
        words[LD_BODY + 0] = static_cast<float>(body.radius);
        const double a[3] = {B[1], B[4], B[7]};
        for (int f = 0; f < 6; ++f) {
            const CubeFace& F = cubeFace(f);
            float* w = words + LD_FACE + f * LD_FACE_WORDS;
            const double* axes[3] = {F.n, F.u, F.v};
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j)
                    w[k * 4 + j] = static_cast<float>(B[j] * axes[k][0] + B[3 + j] * axes[k][1] + B[6 + j] * axes[k][2]);
            const ChartPoint c = chartOf(body, a, f);
            const double tx = c.x / config.tileMetres, tz = c.z / config.tileMetres;
            w[12] = static_cast<float>(std::floor(tx));
            w[13] = static_cast<float>(std::floor(tz));
            w[14] = static_cast<float>(tx - std::floor(tx));
            w[15] = static_cast<float>(tz - std::floor(tz));
        }
    }
    words[LD_RINGS + 0] = r.cellSize;
    words[LD_RINGS + 1] = 4.0f / static_cast<float>(r.resolution);
    words[LD_RINGS + 2] = static_cast<float>(node.ringLevels());
    words[LD_RINGS + 3] = 0.5f * static_cast<float>(r.resolution);
    words[LD_TILES + 0] = config.tileMetres;
    words[LD_TILES + 1] = static_cast<float>(config.tileTexels);
    words[LD_TILES + 2] = static_cast<float>(config.border);
    words[LD_TILES + 3] = static_cast<float>(lt.tiles.levels());
    words[LD_PAGE + 0] = static_cast<float>(lt.tiles.pageShift());
    words[LD_PAGE + 1] = static_cast<float>(config.pageSize);
    words[LD_PAGE + 2] = static_cast<float>(config.maxLevels);
    words[LD_PAGE + 3] = lt.tiles.stats().resident > 0 ? 1.0f : 0.0f;
    const bool canopy = node.canopySourceHash() != 0;
    words[LD_CANOPY + 0] = canopy ? node.canopyRange() : 1e30f;
    words[LD_CANOPY + 1] = canopy ? node.canopyCastRange() : 1e30f;
    words[LD_CANOPY + 2] = canopy ? 1.0f : 0.0f;
}

bool SceneMeshDrawer::grassGround(SceneFrame& frame, const GrassNode& grass, GrassGround& out) {
    static_assert(sizeof(out.words) == LD_WORDS * sizeof(float));
    out = {};
    SceneNode* n = grass.ground() ? frame.graph.findById(grass.ground()) : nullptr;
    LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    if (!land) return false;
    const auto found = lands_.find(land->id());
    if (found == lands_.end() || found->second->failed) return false;
    const LandTiles& lt = *found->second;
    if (lt.tiles.stats().resident == 0 || lt.generation != land->generation()) return false;
    out.land = land;
    for (int i = 0; i < 4; ++i) out.layers[i] = lt.atlas.layerView(i);
    out.sampler = lt.atlas.layerSampler();
    out.page = lt.atlas.pageView();
    out.pageSampler = lt.atlas.pageSampler();
    landWords(frame, *land, lt, out.words);
    return true;
}

bool SceneMeshDrawer::prepareLand(SceneFrame& frame, LandNode& node, MeshDraw& out) {
    const LandShaders* shaders = landShaders(node);
    LandTiles* lt = shaders && !node.effectiveUnlit() ? landTiles(frame, node) : nullptr;
    if (!lt) {
        node.setStats({});
        return false;
    }
    lt->shaders = shaders;
    landDrawn_.push_back(node.id());
    // Nothing to stand on until its first tiles are made (this frame's
    // fills come after the draw lists).
    if (lt->tiles.stats().resident == 0 || lt->generation != node.generation()) return false;

    // The static path's material, turf and lighting, then the land's own.
    if (!prepare(frame, node, out)) return false;
    out.kind = MeshKind::Land;
    // Its vertices are the body's (its transform is ignored): as a caster
    // they are taken relative to the eye on the body.
    std::memcpy(out.push.model, frame.world.relative(bromath::midentity()).data, sizeof(out.push.model));
    // Its ring blocks, placed where this frame's eye centres the rings (in
    // every pass: a probe's or a turf tile's view reads the rings about
    // the frame's eye, whose tiles are the resident ones), so each pass
    // draws the blocks its view reaches. The rings' indices are in block
    // order (LandNode::buildRings), so they are the chunks' own.
    // The rings centre on the eye on the body. (A chunk's bounds are in the
    // draw's own space, its model below taking them to the camera's.)
    // Ring 0 drawn as the first ring whose cells are a pixel from the eye's
    // height, the rings between left out, and the blocks the body's curve
    // hides.
    const bromath::Vec3 eye = frame.world.eye;
    const SceneView& view = frame.world;
    lt->firstRing = node.firstRing(eye, 0.5f * static_cast<float>(std::max(view.height, 1u)) *
                                            std::fabs(view.proj.at(1, 1)));
    lt->chunks.clear();
    for (const LandNode::Block& b : node.blocks()) {
        if (!LandNode::blockDrawn(b, lt->firstRing)) continue;
        bool hidden = false;
        const bromath::AABB3 box = node.blockBounds(b, eye, lt->firstRing, &hidden);
        if (!hidden) lt->chunks.push_back({b.firstIndex, b.indexCount, box});
    }
    out.chunks = &lt->chunks;
    out.chunkIndices = out.indices;
    // Below the plants, grass and herbs that stand on it.
    out.order = MeshDraw::kOrderMid + 2;
    uint32_t flags = static_cast<uint32_t>(out.push.pbrParams[3]);
    flags = (flags & ~mesh_flags::vertexColor(3)) | mesh_flags::vertexColor(2);
    out.push.pbrParams[3] = static_cast<float>(flags);

    // Set 3: its LandDraw and its tiles.
    float words[LD_WORDS];
    landWords(frame, node, *lt, words);
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(words, sizeof(words));
    VkDescriptorSet set = frame.gpu.device.frameSet(frame.gpu.defaults.boneLayout);
    if (set == VK_NULL_HANDLE) return false;
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(4, lt->atlas.layerView(0), lt->atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(5, lt->atlas.layerView(1), lt->atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(6, lt->atlas.pageView(), lt->atlas.pageSampler());
    writer.writeImage(7, lt->atlas.layerView(3), lt->atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.updateSet(device_, set);
    out.vertexSet = set;
    // Past the plants' range its fragments are their canopy, read from the
    // tiles a fragment at a time (mesh.frag's landCanopyAt).
    if (node.canopySourceHash() != 0)
        out.push.pbrParams[3] = static_cast<float>(static_cast<uint32_t>(out.push.pbrParams[3]) | mesh_flags::kCanopy);
    return true;
}

void SceneMeshDrawer::offerLand(SceneFrame& frame, WorldService& service) {
    landOffered_.clear();
    // A land node gone takes its tiles with it.
    for (auto it = lands_.begin(); it != lands_.end();) {
        if (!frame.graph.findById(it->first)) {
            it->second->atlas.destroy(frame.gpu.device, frame.gpu.allocator);
            it = lands_.erase(it);
        } else {
            ++it;
        }
    }

    // The tiles and the horizon are keyed on the world.
    const SceneView& view = frame.world;
    for (const uint32_t id : landDrawn_) {
        const auto found = lands_.find(id);
        if (found == lands_.end() || found->second->failed) continue;
        LandTiles& lt = *found->second;
        SceneNode* n = frame.graph.findById(id);
        LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
        if (!land) continue;

        // A new module, extent or rings: every tile again, over its items.
        if (lt.generation != land->generation()) {
            lt.generation = land->generation();
            lt.tiles.configure(land->tileConfig(), bromath::aempty3(), land->tileMap());
            lt.tiles.setItems(land->items());
            lt.cleared = false;
        }
        bromath::AABB3 bounds = land->chartBounds();
        bounds.min.y -= kDepthMargin;
        bounds.max.y += kDepthMargin;
        if (lt.tiles.setBounds(bounds)) lt.cleared = false;
        if (!lt.tiles.configured()) continue;
        if (!lt.cleared) {
            lt.atlas.clearPages(frame.cmd);
            lt.cleared = true;
        }
        lt.tiles.setGeneration(land->generation());

        SurfaceLayers::View sv;
        sv.eye = view.eye;
        sv.frustum = view.frustum;
        sv.perspective = view.perspective;
        sv.focalPixels = 0.5f * static_cast<float>(std::max(view.height, 1u)) * std::fabs(view.proj.at(1, 1));
        sv.orthoMetresPerPixel =
            2.0f / std::max(std::fabs(view.proj.at(1, 1)) * static_cast<float>(std::max(view.height, 1u)), 1e-6f);
        lt.tiles.update(sv, ++lt.frame);
        for (const SurfaceJob& j : lt.tiles.jobs())
            service.offer({j.key.packed(), id, landKind_, j.error, j.side, j.required});
        landOffered_.push_back(id);
    }
    landDrawn_.clear();
    // The horizon about the eye, from the first land's tiles; with no land,
    // none.
    if (landOffered_.empty())
        horizon_.forget();
    else if (horizon_.stale(view.eye, landChanged_))
        service.offer({0, landOffered_.front(), horizonKind_, 0.0f, 0.0, /*required=*/true});
}

const WorldHorizon::Read* SceneMeshDrawer::landHorizon(SceneFrame& frame) {
    return horizon_.read(frame.gpu.device.context().queue(), frame.world.eye);
}

uint32_t SceneMeshDrawer::recordLandHorizon(SceneFrame& frame, std::span<const WorldJob> granted,
                                            WorldStream& stream) {
    if (granted.empty()) return 0;
    const auto found = lands_.find(granted.front().producer);
    if (found == lands_.end() || found->second->failed) return 0;
    const LandTiles& lt = *found->second;
    SceneNode* n = frame.graph.findById(found->first);
    LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    if (!land || lt.tiles.stats().resident == 0) return 0;

    float words[LD_WORDS];
    landWords(frame, *land, lt, words);
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(words, sizeof(words));
    VkDescriptorSet set = frame.gpu.device.frameSet(horizonSetLayout_);
    const SceneVkBuffer* out = set != VK_NULL_HANDLE ? horizon_.write(frame.gpu.allocator, frame.world.eye) : nullptr;
    if (!out) return 0;
    landChanged_ = false;
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(1, lt.atlas.layerView(0), lt.atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(2, lt.atlas.layerView(1), lt.atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(3, lt.atlas.pageView(), lt.atlas.pageSampler());
    writer.writeBuffer(4, out->buffer, out->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(device_, set);

    VkCommandBuffer cmd = stream.cmd();
    // The tiles and page entries this frame's fills wrote.
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, horizonPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, horizonLayout_, 0, 1, &set, 0, nullptr);
    const float eye[4] = {frame.world.eye.x, frame.world.eye.y, frame.world.eye.z, 0.0f};
    vkCmdPushConstants(cmd, horizonLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(eye), eye);
    vkCmdDispatch(cmd, WH_AZIMUTHS, 1, 1);
    return 1;
}

uint32_t SceneMeshDrawer::recordLand(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream) {
    std::unordered_map<uint32_t, std::vector<uint64_t>> grantedKeys;
    for (const WorldJob& j : granted) grantedKeys[j.producer].push_back(j.key);

    uint32_t recorded = 0;
    for (const uint32_t id : landOffered_) {
        LandTiles& lt = *lands_.at(id);
        SceneNode* n = frame.graph.findById(id);
        LandNode* land = static_cast<MeshNode*>(n)->asLand();
        const auto keys = grantedKeys.find(id);
        lt.tiles.commit(keys != grantedKeys.end() ? std::span<const uint64_t>(keys->second)
                                                  : std::span<const uint64_t>());
        const std::vector<SurfaceFill>& fills = lt.tiles.fills();
        const SurfaceLayerStats& s = lt.tiles.stats();
        LandNode::Stats stats;
        stats.needed = s.needed;
        stats.resident = s.resident;
        stats.missing = s.missing;
        stats.filled = s.filled;
        stats.levels = s.levels;
        stats.shortfall = s.worstError;
        stats.compiled = true;
        land->setStats(stats);
        frame.stats.landFills += static_cast<int>(fills.size());
        if (!fills.empty() || !lt.tiles.pageWrites().empty()) landChanged_ = landSunTiles_ = true;
        recorded += static_cast<uint32_t>(fills.size());

        const VkPipeline pipeline = fills.empty() ? VK_NULL_HANDLE : landFillPipeline(*lt.shaders);
        if (pipeline) {
            VkCommandBuffer cmd = stream.cmd();
            const SurfaceLayerConfig& config = lt.tiles.config();
            // The fills (each its border's corner, its texel, its slot) and
            // the module's data.
            const VkDeviceSize fillBytes = fills.size() * LF_TILE_WORDS * sizeof(float);
            const std::vector<float>& data = land->moduleData();
            const VkDeviceSize dataBytes = std::max<VkDeviceSize>(data.size() * sizeof(float), 16);
            const render::UploadSlice fillSlice = frame.gpu.device.frameUpload(fillBytes, kStorageAlign);
            const render::UploadSlice dataSlice = frame.gpu.device.frameUpload(dataBytes, kStorageAlign);
            VkDescriptorSet set = frame.gpu.device.frameSet(landFillSetLayout_);
            if (fillSlice && dataSlice && set != VK_NULL_HANDLE) {
                auto* f = static_cast<float*>(fillSlice.mapped);
                std::memset(f, 0, fillBytes);
                for (const SurfaceFill& fill : fills) {
                    float* w = f;
                    f += LF_TILE_WORDS;
                    const double texel = fill.side / config.tileTexels;
                    w[2] = static_cast<float>(texel);
                    w[3] = static_cast<float>(fill.slot);
                    if (!land->round()) {
                        w[0] = static_cast<float>(fill.x0 - texel * config.border);
                        w[1] = static_cast<float>(fill.z0 - texel * config.border);
                        continue;
                    }
                    // On a round body from the tile's middle, whose datum
                    // point is the noise's origin (land_fill.comp).
                    const Body& body = land->place()->body();
                    const double R = body.radius;
                    const double half = 0.5 * fill.side;
                    const double mx = fill.x0 + half, mz = fill.z0 + half;
                    w[0] = static_cast<float>(-half - texel * config.border);
                    w[1] = static_cast<float>(-half - texel * config.border);
                    w[4] = static_cast<float>(std::tan(mx / R));
                    w[5] = static_cast<float>(std::tan(mz / R));
                    w[6] = static_cast<float>(std::cos(mx / R));
                    w[7] = static_cast<float>(std::cos(mz / R));
                    const CubeFace& F = cubeFace(fill.key.root);
                    for (int i = 0; i < 3; ++i) {
                        w[8 + i] = static_cast<float>(F.n[i]);
                        w[12 + i] = static_cast<float>(F.u[i]);
                        w[16 + i] = static_cast<float>(F.v[i]);
                    }
                    w[11] = static_cast<float>(R);
                    w[15] = static_cast<float>(fill.key.root);
                    double c[3];
                    dirOf(body, {fill.key.root, mx, mz}, c);
                    for (int i = 0; i < 3; ++i) {
                        const double o = R * c[i];
                        const float hi = static_cast<float>(o);
                        w[20 + 2 * i] = hi;
                        w[21 + 2 * i] = static_cast<float>(o - static_cast<double>(hi));
                    }
                    w[26] = static_cast<float>(mx);
                    w[27] = static_cast<float>(mz);
                }
                std::memset(dataSlice.mapped, 0, dataBytes);
                if (!data.empty()) std::memcpy(dataSlice.mapped, data.data(), data.size() * sizeof(float));
                SceneVkDescriptorWriter writer;
                writer.writeImage(0, lt.atlas.layerView(0), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                writer.writeImage(1, lt.atlas.layerView(1), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                writer.writeBuffer(2, fillSlice.buffer, fillBytes, fillSlice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                writer.writeBuffer(3, dataSlice.buffer, dataBytes, dataSlice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                writer.writeImage(4, lt.atlas.layerView(2), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                writer.writeImage(5, lt.atlas.layerView(3), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                writer.updateSet(device_, set);

                lt.atlas.beginFills(cmd, fills);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, landFillLayout_, 0, 1, &set, 0, nullptr);
                const uint32_t stride = static_cast<uint32_t>(config.stride());
                const uint32_t words[LF_P_WORDS] = {stride, static_cast<uint32_t>(config.border),
                                                    static_cast<uint32_t>(fills.size()), 0u};
                vkCmdPushConstants(cmd, landFillLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(words), words);
                const uint32_t groups = (stride + LF_GROUP - 1) / LF_GROUP;
                vkCmdDispatch(cmd, groups, groups, static_cast<uint32_t>(fills.size()));
                lt.atlas.endFills(cmd, fills);
            }
        }
        if (!lt.tiles.pageWrites().empty()) lt.atlas.writePages(frame.gpu.device, stream.cmd(), lt.tiles.pageWrites());
    }
    return recorded;
}

}  // namespace bro::scene::vk
