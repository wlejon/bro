// The turf's surface layers (docs/scene-world-architecture.md §5): each
// grass node's tiles, chosen for the camera (scene/surface_layers.h), filled
// by drawing its turf meshes straight down into them with
// ground_layer_fill.frag, and found by the turf's fragments through the page
// table (scene_ground_layers.glsl).

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/grass_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"

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
    c.maxLevels = 20;
    c.texelPixels = 1.5f;
    c.fillBudget = 24;
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
                                                          const bromath::AABB3& bounds) {
    // Its key: its geometry and where it is.
    uint64_t key = (1469598103934665603ull ^ node.id()) * 1099511628211ull;
    key = (key ^ node.geometryGeneration()) * 1099511628211ull;
    {
        uint32_t bits[6];
        std::memcpy(bits, &bounds, sizeof(bits));
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
            t.items.push_back({box, (key ^ k) * 0x9e3779b97f4a7c15ull});
        }
    return t.items;
}

SceneMeshDrawer::GroundLayers* SceneMeshDrawer::groundLayers(SceneFrame& frame, const GrassNode& grass) {
    auto& slot = groundLayers_[grass.id()];
    if (!slot) {
        slot = std::make_unique<GroundLayers>();
        const SurfaceLayerConfig config = groundConfig();
        slot->tiles.configure(config, bromath::aempty3());
        if (!slot->atlas.create(frame.gpu.device, frame.gpu.allocator, config, config.maxLevels, kLayerFormats)) {
            LOG_ERROR("SceneMeshDrawer: the turf's surface layer atlas could not be made; the turf works its "
                      "patterns out where it draws");
            slot->failed = true;
        }
    }
    return slot->failed ? nullptr : slot.get();
}

void SceneMeshDrawer::recordGroundLayers(SceneFrame& frame) {
    // Each grass node's turf draws this frame.
    std::unordered_map<uint32_t, std::vector<const MeshDraw*>> turf;
    for (const MeshDraw& d : frame.lists.meshes) {
        if (d.kind != MeshKind::Static || !d.customSet || !d.hasBounds) continue;
        if (!(static_cast<uint32_t>(d.push.pbrParams[3]) & mesh_flags::kGround)) continue;
        const SceneNode* n = frame.graph.findById(d.nodeId);
        if (!n || n->type() != SceneNode::Type::Mesh) continue;
        if (const uint32_t g = static_cast<const MeshNode*>(n)->turfGrass()) turf[g].push_back(&d);
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
    const SceneView& view = frame.view;
    for (auto& [grassId, draws] : turf) {
        const auto found = groundLayers_.find(grassId);
        if (found == groundLayers_.end() || found->second->failed) continue;
        GroundLayers& gl = *found->second;
        // Once a frame (BRO_SCENE_GPU_REPEAT records the graph again: those
        // draw from what the first filled).
        const uint64_t serial = frame.gpu.device.frames().frameSerial();
        if (gl.serial == serial) continue;
        gl.serial = serial;
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
            const auto* n = static_cast<const MeshNode*>(frame.graph.findById(d->nodeId));
            const uint64_t g = n->geometryGeneration();
            mixKey(&d->nodeId, sizeof(d->nodeId));
            mixKey(&g, sizeof(g));
            mixKey(&d->bounds, sizeof(d->bounds));
        }
        if (turfKey != gl.turfKey) {
            gl.turfKey = turfKey;
            std::vector<SurfaceItem> items;
            gl.bounds = bromath::aempty3();
            for (const MeshDraw* d : draws) {
                const auto* n = static_cast<const MeshNode*>(frame.graph.findById(d->nodeId));
                const std::vector<SurfaceItem>& mine = turfItems(gl, *n, d->bounds);
                items.insert(items.end(), mine.begin(), mine.end());
                gl.bounds.min = bromath::vmin(gl.bounds.min, d->bounds.min);
                gl.bounds.max = bromath::vmax(gl.bounds.max, d->bounds.max);
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
        gl.tiles.setGeneration(groundGeneration(*grass));

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
        const std::vector<SurfaceFill>& fills = gl.tiles.fills();
        frame.stats.groundLayerFills += static_cast<int>(fills.size());
        frame.stats.groundLayerNeeded += static_cast<int>(gl.tiles.stats().needed);
        frame.stats.groundLayerMissing += static_cast<int>(gl.tiles.stats().missing);
        frame.stats.groundLayerShortfall = std::max(frame.stats.groundLayerShortfall, gl.tiles.stats().worstError);

        if (!fills.empty()) {
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
                SceneCameraUniforms cam = view.uniforms(&frame.renderer);
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
                    if (d->bounds.max.x < box.min.x || d->bounds.min.x > box.max.x || d->bounds.max.z < box.min.z ||
                        d->bounds.min.z > box.max.z)
                        continue;
                    MeshDraw c = *d;
                    c.twoSided = true;   // (seen from above, whichever way its faces wind)
                    record(cmd, target, cameraSet, frame.lightingSet, c, &cull);
                }
                frame.gpu.device.cmdEndRendering(cmd);
            }
            atlas.endFills(cmd, fills);
        }
        gl.atlas.writePages(frame.gpu.device, cmd, gl.tiles.pageWrites());
    }
}

}  // namespace bro::scene::vk
