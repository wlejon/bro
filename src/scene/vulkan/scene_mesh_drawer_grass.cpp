// SceneMeshDrawer's grass path: a GrassNode becomes up to three draws, one
// per ring around the camera, each instancing the blade mesh over only the
// cells of its ring the view can see. The shader (shaders/scene_grass.glsl)
// places, culls, bends and colours every blade; this file picks the cells,
// stamps the movers and binds the node's field and influence map.

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/grass_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

/// shaders/scene_grass.glsl's GrassUBO.
struct GrassUBO {
    float cells[4];
    float ring[4];
    float field[4];
    float fieldSize[4];
    float blade[4];
    float lush[4];
    float dry[4];
    float look[4];
    float push[4];
    float pushRate[4];
};

constexpr float kPi = 3.14159265358979f;
constexpr float kWidthGrowth = 0.03f;            // a far blade is wider, so a ring keeps its cover
constexpr uint32_t kMaxRingInstances = 1u << 22;  // a guard, far above any view's need

/// The x/z box around the eye holding everything of a disc of `radius` the
/// view can see (the view's cone flattened onto the ground), padded by
/// `margin`. The whole disc when the camera looks steeply down or is
/// orthographic.
void visibleBox(const SceneView& view, float radius, float margin, float (&box)[4]) {
    const float ex = view.eye.x, ez = view.eye.z;
    box[0] = ex - radius - margin;
    box[1] = ez - radius - margin;
    box[2] = ex + radius + margin;
    box[3] = ez + radius + margin;
    if (std::fabs(view.proj.at(3, 3)) > 0.5f) return;   // orthographic

    // The four corner rays of the frustum, as x/z angles.
    std::array<float, 4> ang{};
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; ++k) {
        const bromath::Mat4& ip = view.invProj;
        float p[4];
        for (int r = 0; r < 4; ++r)
            p[r] = ip.at(r, 0) * corners[k][0] + ip.at(r, 1) * corners[k][1] + ip.at(r, 2) * 0.5f + ip.at(r, 3);
        if (std::fabs(p[3]) < 1e-12f) return;
        const float vx = p[0] / p[3], vy = p[1] / p[3], vz = p[2] / p[3];
        const bromath::Mat4& iv = view.invRelView;
        const float dx = iv.at(0, 0) * vx + iv.at(0, 1) * vy + iv.at(0, 2) * vz;
        const float dz = iv.at(2, 0) * vx + iv.at(2, 1) * vy + iv.at(2, 2) * vz;
        if (dx * dx + dz * dz < 1e-8f) return;
        ang[k] = std::atan2(dz, dx);
    }
    // The smallest arc holding all four: the complement of the widest gap.
    std::sort(ang.begin(), ang.end());
    float gap = ang[0] + 2.0f * kPi - ang[3];
    float start = ang[0];
    for (int k = 0; k < 3; ++k) {
        if (ang[k + 1] - ang[k] > gap) {
            gap = ang[k + 1] - ang[k];
            start = ang[k + 1];
        }
    }
    const float arc = 2.0f * kPi - gap;
    if (arc >= kPi * 0.98f) return;

    float lo[2] = {ex, ez}, hi[2] = {ex, ez};
    const auto take = [&](float a) {
        const float x = ex + radius * std::cos(a), z = ez + radius * std::sin(a);
        lo[0] = std::min(lo[0], x);
        lo[1] = std::min(lo[1], z);
        hi[0] = std::max(hi[0], x);
        hi[1] = std::max(hi[1], z);
    };
    take(start);
    take(start + arc);
    for (int q = 0; q < 4; ++q) {
        const float a = static_cast<float>(q) * 0.5f * kPi;
        float rel = std::fmod(a - start, 2.0f * kPi);
        if (rel < 0.0f) rel += 2.0f * kPi;
        if (rel <= arc) take(a);
    }
    box[0] = lo[0] - margin;
    box[1] = lo[1] - margin;
    box[2] = hi[0] + margin;
    box[3] = hi[1] + margin;
}

MeshNode::UserTexture* slot(GrassNode& node, const char* name) {
    for (auto& t : node.customShaderTextures())
        if (t.name == name) return &t;
    return nullptr;
}

}  // namespace

void SceneMeshDrawer::prepareGrass(SceneFrame& frame, GrassNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    const SceneView& view = frame.view;
    node.stamp(frame.graph, frame.renderer.windTime(), view.eye.x, view.eye.z);
    for (int i = 0; i < GrassNode::kRings; ++i) node.setDrawnInstances(i, 0);
    if (!node.hasField() || node.effectiveUnlit()) return;

    SceneGpuResources& res = frame.gpu.resources;
    MeshNode::UserTexture* fieldTex = slot(node, GrassNode::kFieldTexture);
    MeshNode::UserTexture* pushTex = slot(node, GrassNode::kPushTexture);
    const SceneVkImage* field = fieldTex ? res.userTexture(node.id(), *fieldTex) : nullptr;
    const SceneVkImage* push = pushTex ? res.userTexture(node.id(), *pushTex) : nullptr;
    res.pruneUserTextures(node.id(), node.customShaderTextures());
    if (!field || !push) return;

    const GpuMesh* nearBlade = res.mesh(node.id(), 0, node.geometryGeneration(), node.mesh());
    const GpuMesh* farBlade = res.mesh(node.id(), 1, node.geometryGeneration(), node.farBlade());
    if (!nearBlade || !farBlade) return;

    const GrassNode::Params& p = node.params();
    GrassUBO u{};
    u.field[0] = node.fieldX0();
    u.field[1] = node.fieldZ0();
    u.field[2] = node.fieldCell();
    u.fieldSize[0] = static_cast<float>(node.fieldCols());
    u.fieldSize[1] = static_cast<float>(node.fieldRows());
    u.fieldSize[2] = 1.0f / static_cast<float>(node.fieldCols());
    u.fieldSize[3] = 1.0f / static_cast<float>(node.fieldRows());
    std::copy(std::begin(p.lush), std::end(p.lush), u.lush);
    u.lush[3] = p.bulk;
    std::copy(std::begin(p.dry), std::end(p.dry), u.dry);
    u.dry[3] = p.clumpFreq;
    u.look[0] = p.rootShade;
    u.look[1] = p.sheen;
    u.look[2] = p.variety;
    u.look[3] = kWidthGrowth;
    u.push[0] = node.pushX0();
    u.push[1] = node.pushZ0();
    u.push[2] = GrassNode::kPushCell;
    u.push[3] = static_cast<float>(GrassNode::kPushCells);
    u.pushRate[0] = GrassNode::kBendRecovery;
    u.pushRate[1] = GrassNode::kTrampleRecovery;

    uint32_t flags = 0;
    if (frame.ssr) flags |= mesh_flags::kReflectance;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    flags |= mesh_flags::kTwoSided;

    for (int r = 0; r < GrassNode::kRings; ++r) {
        const GrassNode::Ring ring = node.ring(r);
        // Blades lean and stand up to ~2x the mean height: pad the box by
        // that, a cell and a margin.
        float box[4];
        visibleBox(view, ring.outer, ring.cell * 2.0f + p.height * 3.0f + 1.0f, box);
        const int i0 = static_cast<int>(std::floor(box[0] / ring.cell));
        const int j0 = static_cast<int>(std::floor(box[1] / ring.cell));
        const int i1 = static_cast<int>(std::floor(box[2] / ring.cell));
        const int j1 = static_cast<int>(std::floor(box[3] / ring.cell));
        const uint64_t w = static_cast<uint64_t>(std::max(i1 - i0 + 1, 0));
        const uint64_t h = static_cast<uint64_t>(std::max(j1 - j0 + 1, 0));
        if (w == 0 || h == 0 || w * h > kMaxRingInstances) continue;

        u.cells[0] = static_cast<float>(i0);
        u.cells[1] = static_cast<float>(j0);
        u.cells[2] = static_cast<float>(w);
        u.cells[3] = ring.cell;
        u.ring[0] = ring.inner;
        u.ring[1] = ring.outer;
        u.ring[2] = ring.bandIn;
        u.ring[3] = ring.bandOut;
        u.blade[0] = p.height;
        u.blade[1] = ring.width;
        u.blade[2] = p.minNormalY;
        u.blade[3] = p.wind;

        const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&u, sizeof(u));
        VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->customLayout);
        if (set == VK_NULL_HANDLE) return;
        SceneVkDescriptorWriter writer;
        writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
        writer.writeImage(1, field->view, field->sampler);
        writer.writeImage(2, push->view, push->sampler);
        for (uint32_t b = 3; b <= static_cast<uint32_t>(MeshNode::kMaxUserTextures); ++b)
            writer.writeImage(b, defaults_->white.view, defaults_->sampler);
        writer.updateSet(device_, set);

        const GpuMesh* blade = r == 0 ? nearBlade : farBlade;
        MeshDraw draw;
        draw.kind = MeshKind::Grass;
        draw.nodeId = node.id();
        draw.vertices = blade->vertices.buffer;
        draw.indices = blade->indices.buffer;
        draw.indexCount = blade->indexCount;
        draw.instanceCount = static_cast<uint32_t>(w * h);
        draw.customSet = set;
        std::memcpy(draw.push.model, view.relative(node.worldMatrix()).data, sizeof(draw.push.model));
        std::memcpy(draw.push.baseColor, node.color(), sizeof(draw.push.baseColor));
        draw.push.baseColor[3] = 1.0f;
        draw.push.pbrParams[0] = node.metallic();
        draw.push.pbrParams[1] = node.roughness();
        draw.push.pbrParams[3] = static_cast<float>(flags);
        draw.push.extra[1] = node.subsurface();
        draw.push.extra[3] = 1.0f + 256.0f;   // no atlas
        draw.twoSided = true;
        // Only the near ring casts: its blades are the ones whose shadows read.
        draw.castsShadow = r == 0 && node.castsShadow();
        out.push_back(draw);
        node.setDrawnInstances(r, draw.instanceCount);
    }
}

}  // namespace bro::scene::vk
