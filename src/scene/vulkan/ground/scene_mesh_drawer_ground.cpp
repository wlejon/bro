// The turf's surface layers: each
// grass node's tiles, chosen for the camera (scene/world/surface_layers.h),
// offered to the world service (§6, scene/vulkan/world/scene_world_service.h)
// as fills, filled as it grants them by drawing the turf meshes straight
// down into them with ground_layer_fill.frag, and found by the turf's
// fragments through the page table (scene_ground_layers.glsl). On a round
// body the tiles are squares of the land's chart (its six faces the roots),
// each filled whole by the fill's GROUND_LAYER_CHART build.

#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "scene/nodes/grass_node.h"
#include "scene/nodes/land_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_depth.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/ground/scene_cover_shaders.h"
#include "scene/world/place.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_map>

namespace bro::scene::vk {

namespace {

// The tiles' layers (GroundBake's a, b, c; d holds the blooms' colour).
constexpr VkFormat kLayerFormats[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_FORMAT_R8G8B8A8_SRGB};
// How far above and below its meshes' boxes a tile's camera reaches.
constexpr float kDepthMargin = 10.0f;

SurfaceLayerConfig groundConfig() {
    SurfaceLayerConfig c;
    c.tileMetres = 1.0f;
    c.tileTexels = 64;
    c.border = 1;
    c.slots = 1024;
    c.pageSize = 256;
    // Enough that a round body's cube face (its chart's side a quarter of
    // the great circle: 10,000 km for the Earth's) is a coarsest tile or
    // two. At 20 the coarsest of a metre's tiles were 524 km: the planet's
    // faces 2,400 of them, all kept whatever the view, more than the
    // slots, so none refined and a fragment either read a 524 km tile's
    // texel (every pattern at its mean) or, where none was held, worked
    // all of them out itself (a fifth of the turf's cost seen from 2 km).
    c.maxLevels = 26;
    c.texelPixels = 1.5f;
    return c;
}

/// The camera-relative matrix taking a turf vertex (world - eye) to a fill's
/// clip space: x and z across the tile and its border, the highest ground
/// nearest (reversed-Z: the highest ground at 1).
bromath::Mat4 fillViewProj(const SurfaceFill& f, double texel, int border, const bromath::Vec3& eye, float yMin,
                           float yMax) {
    const double pad = texel * border;
    const double x0 = f.x0 - pad, z0 = f.z0 - pad, side = f.side + 2.0 * pad;
    const double s = 2.0 / side;
    const double yr = std::max(static_cast<double>(yMax) - yMin, 1e-3);
    bromath::Mat4 m = bromath::midentity();
    m.at(0, 0) = static_cast<float>(s);
    m.at(0, 1) = 0.0f;
    m.at(0, 2) = 0.0f;
    m.at(0, 3) = static_cast<float>((static_cast<double>(eye.x) - x0) * s - 1.0);
    m.at(1, 0) = 0.0f;
    m.at(1, 1) = 0.0f;
    m.at(1, 2) = static_cast<float>(s);
    m.at(1, 3) = static_cast<float>((static_cast<double>(eye.z) - z0) * s - 1.0);
    m.at(2, 0) = 0.0f;
    m.at(2, 2) = 0.0f;
    m.at(2, 1) = static_cast<float>(1.0 / yr);
    m.at(2, 3) = static_cast<float>((static_cast<double>(eye.y) - yMin) / yr);
    m.at(3, 0) = m.at(3, 1) = m.at(3, 2) = 0.0f;
    m.at(3, 3) = 1.0f;
    return m;
}

// A turf mesh's items are its ground's heights in cells about this size.
constexpr float kItemMetres = 32.0f;
constexpr int kItemCellsMax = 128;   // a side, for a mesh past 4 km

}  // namespace

const std::vector<SurfaceItem>& SceneMeshDrawer::turfItems(GroundLayers& gl, const MeshNode& node,
                                                          const bromath::AABB3& bounds,
                                                          const bromath::Vec3& origin) {
    // Its key: its geometry and where it is on the body (so a re-anchor,
    // which moves it in the place frame alone, refills nothing).
    uint64_t key = (1469598103934665603ull ^ node.id()) * 1099511628211ull;
    key = (key ^ node.geometryGeneration()) * 1099511628211ull;
    {
        const bromath::AABB3 body{bounds.min + origin, bounds.max + origin};
        uint32_t bits[6];
        std::memcpy(bits, &body, sizeof(bits));
        for (uint32_t b : bits) key = (key ^ b) * 1099511628211ull;
    }
    GroundLayers::TurfItems& t = gl.turf[node.id()];
    if (t.key == key) return t.items;
    t.key = key;
    t.items.clear();

    // Its triangles' heights, each in the cells its box covers (the finest
    // mesh: what the drawn level of detail is cannot change the ground).
    const bromesh::MeshData& data = node.mesh();
    const float ex = bounds.max.x - bounds.min.x, ez = bounds.max.z - bounds.min.z;
    const float cell = std::max({kItemMetres, ex / kItemCellsMax, ez / kItemCellsMax});
    const int nx = std::max(1, static_cast<int>(std::ceil(ex / cell)));
    const int nz = std::max(1, static_cast<int>(std::ceil(ez / cell)));
    std::vector<float> lo(static_cast<size_t>(nx) * nz, std::numeric_limits<float>::max());
    std::vector<float> hi(lo.size(), -std::numeric_limits<float>::max());
    const bromath::Mat4& w = node.worldMatrix();
    const size_t vc = data.vertexCount();
    const auto at = [&](uint32_t v) {
        const float* p = &data.positions[static_cast<size_t>(v) * 3];
        return bromath::Vec3{w.at(0, 0) * p[0] + w.at(0, 1) * p[1] + w.at(0, 2) * p[2] + w.at(0, 3),
                             w.at(1, 0) * p[0] + w.at(1, 1) * p[1] + w.at(1, 2) * p[2] + w.at(1, 3),
                             w.at(2, 0) * p[0] + w.at(2, 1) * p[1] + w.at(2, 2) * p[2] + w.at(2, 3)};
    };
    const size_t ic = data.indices.empty() ? vc : data.indices.size();
    const auto clampCell = [](float v, int n) { return std::clamp(static_cast<int>(v), 0, n - 1); };
    for (size_t i = 0; i + 2 < ic; i += 3) {
        const auto idx = [&](size_t k) { return data.indices.empty() ? static_cast<uint32_t>(k) : data.indices[k]; };
        const bromath::Vec3 a = at(idx(i)), b = at(idx(i + 1)), c = at(idx(i + 2));
        const int i0 = clampCell((std::min({a.x, b.x, c.x}) - bounds.min.x) / cell, nx);
        const int i1 = clampCell((std::max({a.x, b.x, c.x}) - bounds.min.x) / cell, nx);
        const int j0 = clampCell((std::min({a.z, b.z, c.z}) - bounds.min.z) / cell, nz);
        const int j1 = clampCell((std::max({a.z, b.z, c.z}) - bounds.min.z) / cell, nz);
        const float ylo = std::min({a.y, b.y, c.y}), yhi = std::max({a.y, b.y, c.y});
        for (int j = j0; j <= j1; ++j)
            for (int x = i0; x <= i1; ++x) {
                const size_t k = static_cast<size_t>(j) * nx + x;
                lo[k] = std::min(lo[k], ylo);
                hi[k] = std::max(hi[k], yhi);
            }
    }
    for (int j = 0; j < nz; ++j)
        for (int x = 0; x < nx; ++x) {
            const size_t k = static_cast<size_t>(j) * nx + x;
            if (lo[k] > hi[k]) continue;
            const bromath::AABB3 box{{bounds.min.x + x * cell, lo[k], bounds.min.z + j * cell},
                                     {std::min(bounds.max.x, bounds.min.x + (x + 1) * cell), hi[k],
                                      std::min(bounds.max.z, bounds.min.z + (j + 1) * cell)}};
            t.items.push_back({{box.min + origin, box.max + origin}, (key ^ k) * 0x9e3779b97f4a7c15ull});
        }
    return t.items;
}

namespace {

/// The land a grass node stands on when it is a round body's: its turf's
/// tiles are that land's chart's.
const LandNode* roundLand(SceneGraph& graph, const GrassNode& grass) {
    SceneNode* n = grass.ground() ? graph.findById(grass.ground()) : nullptr;
    const LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    return land && land->tileMap() ? land : nullptr;
}

}  // namespace

SceneMeshDrawer::GroundLayers* SceneMeshDrawer::groundLayers(SceneFrame& frame, const GrassNode& grass) {
    auto& slot = groundLayers_[grass.id()];
    const LandNode* land = roundLand(frame.graph, grass);
    const int roots = land ? land->tileMap()->roots() : 1;
    if (slot && slot->roots != roots) {
        slot->atlas.destroy(frame.gpu.device, frame.gpu.allocator);
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<GroundLayers>();
        slot->roots = roots;
        SurfaceLayerConfig config = groundConfig();
        if (land) {
            // On the land's chart, a land tile a power of two of the turf's
            // (scene_ground_layers.glsl's groundChartPosOf).
            const float t = land->tileConfig().tileMetres;
            config.tileMetres = t / std::exp2(std::round(std::log2(t / config.tileMetres)));
        }
        slot->tiles.configure(config, bromath::aempty3(), land ? land->tileMap() : nullptr);
        if (!slot->atlas.create(frame.gpu.device, frame.gpu.allocator, config, config.maxLevels * roots,
                                kLayerFormats)) {
            LOG_ERROR("SceneMeshDrawer: the turf's surface layer atlas could not be made; the turf works its "
                      "patterns out where it draws");
            slot->failed = true;
        }
    }
    return slot->failed ? nullptr : slot.get();
}

void SceneMeshDrawer::offerTurf(SceneFrame& frame, WorldService& service) {
    // Each grass node's turf draws this frame (static meshes and lands).
    turfDraws_.clear();
    turfOffered_.clear();
    for (const MeshDraw& d : frame.lists.meshes) {
        if ((d.kind != MeshKind::Static && d.kind != MeshKind::Land) || !d.customSet || !d.hasBounds) continue;
        if (!(static_cast<uint32_t>(d.push.pbrParams[3]) & mesh_flags::kGround)) continue;
        const SceneNode* n = frame.graph.findById(d.nodeId);
        if (!n || n->type() != SceneNode::Type::Mesh) continue;
        if (const uint32_t g = static_cast<const MeshNode*>(n)->turfGrass()) turfDraws_[g].push_back(&d);
    }
    // A grass node gone takes its layers with it.
    for (auto it = groundLayers_.begin(); it != groundLayers_.end();) {
        if (!frame.graph.findById(it->first)) {
            it->second->atlas.destroy(frame.gpu.device, frame.gpu.allocator);
            it = groundLayers_.erase(it);
        } else {
            ++it;
        }
    }

    VkCommandBuffer cmd = frame.cmd;
    // The turf's tiles are keyed on the body; its meshes' draws are in the
    // place frame and convert.
    const SceneView& view = frame.world;
    const bromath::Vec3 origin{static_cast<float>(frame.view.origin[0]), static_cast<float>(frame.view.origin[1]),
                               static_cast<float>(frame.view.origin[2])};
    for (auto& [grassId, draws] : turfDraws_) {
        const auto found = groundLayers_.find(grassId);
        if (found == groundLayers_.end() || found->second->failed) continue;
        GroundLayers& gl = *found->second;
        SceneNode* gn = frame.graph.findById(grassId);
        MeshNode* gm = gn && gn->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(gn) : nullptr;
        GrassNode* grass = gm ? gm->asGrass() : nullptr;
        if (!grass) continue;

        // The tiles' items: each turf mesh's chunks where it is chunked
        // (their heights are the ground's there), else the mesh, retaken
        // when the turf drawn changes (a mesh shown or hidden, moved or
        // rebuilt). A tile is stale when the items under it change, so a
        // near mesh shown as the eye nears it refills only the tiles over it.
        uint64_t turfKey = 1469598103934665603ull;
        const auto mixKey = [&turfKey](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) turfKey = (turfKey ^ b[i]) * 1099511628211ull;
        };
        std::sort(draws.begin(), draws.end(), [](const MeshDraw* a, const MeshDraw* b) { return a->nodeId < b->nodeId; });
        for (const MeshDraw* d : draws) {
            auto* n = static_cast<MeshNode*>(frame.graph.findById(d->nodeId));
            const LandNode* land = n->asLand();
            const uint64_t g = land ? land->generation() : n->geometryGeneration();
            const bromath::AABB3 db = frame.view.toBody(d->bounds);
            mixKey(&d->nodeId, sizeof(d->nodeId));
            mixKey(&g, sizeof(g));
            mixKey(&db, sizeof(db));
            // (A land's items move without its generation, as its regions
            // come in and out: the turf's tiles over them are stale then,
            // their heights a coarser field's, terraces under the grass.)
            if (land) {
                const uint64_t s = land->itemsSerial();
                mixKey(&s, sizeof(s));
            }
        }
        if (const LandNode* land = roundLand(frame.graph, *grass)) {
            // On a round body the tiles are the land's chart's, over its
            // items (the meshes on it read them as they are).
            if (turfKey != gl.turfKey) {
                gl.turfKey = turfKey;
                gl.tiles.setItems(land->items());
                gl.turf.clear();
            }
            gl.bounds = land->chartBounds();
        } else if (turfKey != gl.turfKey) {
            gl.turfKey = turfKey;
            std::vector<SurfaceItem> items;
            gl.bounds = bromath::aempty3();
            for (const MeshDraw* d : draws) {
                auto* n = static_cast<MeshNode*>(frame.graph.findById(d->nodeId));
                // (A land's are its own: its mesh is the rings.)
                const LandNode* land = n->asLand();
                const std::vector<SurfaceItem>& mine = land ? land->items() : turfItems(gl, *n, d->bounds, origin);
                items.insert(items.end(), mine.begin(), mine.end());
                const bromath::AABB3 db = frame.view.toBody(d->bounds);
                gl.bounds.min = bromath::vmin(gl.bounds.min, db.min);
                gl.bounds.max = bromath::vmax(gl.bounds.max, db.max);
            }
            // The meshes gone from the graph take their items with them.
            for (auto it = gl.turf.begin(); it != gl.turf.end();)
                it = frame.graph.findById(it->first) ? std::next(it) : gl.turf.erase(it);
            gl.tiles.setItems(std::move(items));
        }
        bromath::AABB3 bounds = gl.bounds;
        bounds.min.y -= kDepthMargin;
        bounds.max.y += kDepthMargin;
        if (gl.tiles.setBounds(bounds)) gl.atlas.clearPages(cmd);
        if (!gl.tiles.configured()) continue;
        gl.tiles.setGeneration(groundGeneration(frame.graph, *grass));

        SurfaceLayers::View sv;
        sv.eye = view.eye;
        sv.frustum = view.frustum;
        sv.perspective = view.perspective;
        sv.focalPixels = 0.5f * static_cast<float>(std::max(view.height, 1u)) * std::fabs(view.proj.at(1, 1));
        sv.orthoMetresPerPixel =
            2.0f / std::max(std::fabs(view.proj.at(1, 1)) * static_cast<float>(std::max(view.height, 1u)), 1e-6f);
        // (Not culled by the pyramid read back to the CPU: on the wilds the
        // shortfall is the visible tiles', and tiles behind swaying trees
        // came and went, five times the fills and nine tiles a frame coarse.)
        gl.tiles.update(sv, ++gl.frame);
        for (const SurfaceJob& j : gl.tiles.jobs())
            service.offer({j.key.packed(), grassId, turfKind_, j.error, j.side, j.required});
        turfOffered_.push_back(grassId);
    }
}

uint32_t SceneMeshDrawer::recordTurf(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream) {
    // The fills granted, by grass node, in rank order.
    std::unordered_map<uint32_t, std::vector<uint64_t>> grantedKeys;
    for (const WorldJob& j : granted) grantedKeys[j.producer].push_back(j.key);

    // A tile's camera and box are the body's (its meshes' bounds convert).
    const SceneView& view = frame.world;
    uint32_t recorded = 0;
    for (const uint32_t grassId : turfOffered_) {
        GroundLayers& gl = *groundLayers_.at(grassId);
        const std::vector<const MeshDraw*>& draws = turfDraws_.at(grassId);
        const auto keys = grantedKeys.find(grassId);
        gl.tiles.commit(keys != grantedKeys.end() ? std::span<const uint64_t>(keys->second)
                                                  : std::span<const uint64_t>());
        const std::vector<SurfaceFill>& fills = gl.tiles.fills();
        frame.stats.groundLayerFills += static_cast<int>(fills.size());
        frame.stats.groundLayerNeeded += static_cast<int>(gl.tiles.stats().needed);
        frame.stats.groundLayerMissing += static_cast<int>(gl.tiles.stats().missing);
        frame.stats.groundLayerShortfall = std::max(frame.stats.groundLayerShortfall, gl.tiles.stats().worstError);
        recorded += static_cast<uint32_t>(fills.size());
        const bromath::AABB3& bounds = gl.tiles.bounds();

        if (!fills.empty()) {
            VkCommandBuffer cmd = stream.cmd();
            SceneSurfaceLayerAtlas& atlas = gl.atlas;
            const TargetFormat target = atlas.target();
            const uint32_t stride = atlas.stride();
            const SurfaceLayerConfig& config = gl.tiles.config();
            atlas.beginFills(cmd, fills);
            for (const SurfaceFill& f : fills) {
                atlas.beginDepth(cmd);
                VkRenderingAttachmentInfo colors[4]{};
                for (int i = 0; i < atlas.layerCount(); ++i) {
                    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colors[i].imageView = atlas.slotView(frame.gpu.device, i, f.slot);
                    colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                }
                VkRenderingAttachmentInfo depth{};
                depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth.imageView = atlas.depth().view;
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depth.clearValue.depthStencil = {depth::clearFar(), 0};
                VkRenderingInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                info.renderArea = {{0, 0}, {stride, stride}};
                info.layerCount = 1;
                info.colorAttachmentCount = static_cast<uint32_t>(atlas.layerCount());
                info.pColorAttachments = colors;
                info.pDepthAttachment = &depth;
                frame.gpu.device.cmdBeginRendering(cmd, &info);
                const VkViewport viewport{0.0f, 0.0f, static_cast<float>(stride), static_cast<float>(stride),
                                          0.0f, 1.0f};
                const VkRect2D scissor{{0, 0}, {stride, stride}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                // The tile's camera: the frame's, its projection straight
                // down onto the tile.
                const double texel = f.side / config.tileTexels;
                SceneCameraUniforms cam = frame.view.uniforms(&frame.renderer);
                const bromath::Mat4 rel = fillViewProj(f, texel, config.border, view.eye, bounds.min.y, bounds.max.y);
                std::memcpy(cam.viewProj, rel.data, sizeof(cam.viewProj));
                const VkDescriptorBufferInfo camInfo = frame.gpu.device.frameUniform(&cam, sizeof(cam));
                VkDescriptorSet cameraSet = frame.gpu.device.frameSet(frame.gpu.defaults.cameraLayout);
                if (cameraSet == VK_NULL_HANDLE) {
                    frame.gpu.device.cmdEndRendering(cmd);
                    continue;
                }
                SceneVkDescriptorWriter camWriter;
                camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
                camWriter.updateSet(frame.gpu.device.device(), cameraSet);

                if (gl.roots > 1) {
                    // A square of the land's chart, filled whole: its corner
                    // from the anchor's point on its face (where the anchor
                    // is near enough to it to be precise from there, as
                    // scene_land.glsl's landChartOff takes it), its texel and
                    // border, its face.
                    const MeshDraw& turf = *draws.front();
                    VkPipeline pipeline = chartFillPipeline(
                        *covers_[turf.moduleSet < covers_.size() ? turf.moduleSet : 0], target);
                    if (pipeline == VK_NULL_HANDLE || !turf.customSet) {
                        frame.gpu.device.cmdEndRendering(cmd);
                        continue;
                    }
                    const Place& place = frame.graph.place();
                    const double* B = place.basis();
                    const double up[3] = {B[1], B[4], B[7]};
                    const int face = f.key.root;
                    const CubeFace& F = cubeFace(face);
                    const bool near = F.n[0] * up[0] + F.n[1] * up[1] + F.n[2] * up[2] > 0.1;
                    double ox = f.x0, oz = f.z0;
                    if (near) {
                        const ChartPoint a = chartOf(place.body(), up, face);
                        ox -= a.x;
                        oz -= a.z;
                    }
                    MeshPushConstants push{};
                    push.model[0] = static_cast<float>(ox);
                    push.model[1] = static_cast<float>(oz);
                    push.model[2] = static_cast<float>(texel);
                    push.model[3] = static_cast<float>(config.border);
                    push.model[4] = static_cast<float>(face);
                    push.model[5] = near ? 1.0f : 0.0f;
                    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &cameraSet, 0,
                                            nullptr);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &turf.customSet, 0,
                                            nullptr);
                    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(MeshPushConstants), &push);
                    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                    frame.gpu.device.cmdEndRendering(cmd);
                    continue;
                }

                // The tile's box, for its meshes and their chunks.
                const double pad = texel * config.border;
                bromath::AABB3 box{{static_cast<float>(f.x0 - pad), bounds.min.y, static_cast<float>(f.z0 - pad)},
                                   {static_cast<float>(f.x0 + f.side + pad), bounds.max.y,
                                    static_cast<float>(f.z0 + f.side + pad)}};
                bromath::Frustum tileFrustum;
                {
                    // Its planes: the box's six faces, facing in.
                    const bromath::Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    const float d[6] = {-box.min.x, box.max.x, -box.min.y, box.max.y, -box.min.z, box.max.z};
                    for (int p = 0; p < 6; ++p) tileFrustum.planes[p] = {n[p], d[p]};
                }
                const ChunkCull cull{&tileFrustum, view.eye, false};
                for (const MeshDraw* d : draws) {
                    const bromath::AABB3 db = frame.view.toBody(d->bounds);
                    if (db.max.x < box.min.x || db.min.x > box.max.x || db.max.z < box.min.z || db.min.z > box.max.z)
                        continue;
                    MeshDraw c = *d;
                    c.twoSided = true;   // (seen from above, whichever way its faces wind)
                    record(cmd, target, cameraSet, frame.lightingSet, c, &cull);
                }
                frame.gpu.device.cmdEndRendering(cmd);
            }
            atlas.endFills(cmd, fills);
        }
        if (!gl.tiles.pageWrites().empty()) gl.atlas.writePages(frame.gpu.device, stream.cmd(), gl.tiles.pageWrites());
    }
    return recorded;
}

}  // namespace bro::scene::vk
