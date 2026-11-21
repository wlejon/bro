#include "eye/eye_form.h"
#include "eye/eye_color.h"

#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_view_inspect.h"

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::eye {
namespace {

std::string toLowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isGroundLike(const scene::MeshNode* mn, const bromath::AABB3& wb, double maxFootprint, double minY) {
    const std::string lower = toLowerCopy(mn->name());
    if (lower == "ground" || lower.find("ground_") != std::string::npos ||
        lower.find("_ground") != std::string::npos || lower.find("floor") != std::string::npos ||
        lower.find("terrain") != std::string::npos || lower.find("backdrop") != std::string::npos ||
        lower.find("studio") != std::string::npos) {
        return true;
    }
    const double xzSpan = std::max(wb.max.x - wb.min.x, wb.max.z - wb.min.z);
    const double yThick = wb.max.y - wb.min.y;
    return maxFootprint > 1e-4 && xzSpan > 0.7 * maxFootprint && yThick < 0.2 * xzSpan && wb.min.y <= minY + 0.05;
}

}  // namespace

EyeFormResult analyzeSceneForm(scene::SceneGraph& graph, const EyeFormOptions& opts) {
    EyeFormResult res;
    if (opts.render) graph.renderForRead();

    struct Cand {
        scene::MeshNode* node = nullptr;
        bromath::AABB3 wb;
        std::string name;
    };
    std::vector<Cand> cands;
    double minY = 1e18, maxFootprint = 0.0;
    graph.root()->traverse([&](scene::SceneNode* n) {
        if (!n || !n->renderVisible() || n->type() != scene::SceneNode::Type::Mesh) return;
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (!mn->hasDrawableMesh() || mn->drawMode() != scene::MeshNode::DrawMode::Triangles) return;
        const bromath::AABB3 wb = bromath::atransform(mn->localBounds(), mn->worldMatrix());
        if (bromath::aisEmpty(wb)) return;
        minY = std::min(minY, static_cast<double>(wb.min.y));
        maxFootprint = std::max(maxFootprint, static_cast<double>(std::max(wb.max.x - wb.min.x, wb.max.z - wb.min.z)));
        cands.push_back({mn, wb, mn->name()});
    });

    const std::string focusLow = toLowerCopy(opts.focus);
    std::vector<const Cand*> selected;
    for (const auto& c : cands) {
        if (!focusLow.empty()) {
            const std::string nl = toLowerCopy(c.name);
            if (nl == focusLow || nl.find(focusLow) != std::string::npos) {
                selected.push_back(&c);
            }
        } else if (!isGroundLike(c.node, c.wb, maxFootprint, minY)) {
            selected.push_back(&c);
        }
    }
    if (selected.empty() && !cands.empty()) selected.push_back(&cands[0]);
    if (selected.empty()) {
        res.ok = false;
        return res;
    }

    // Merge selected world-space triangles cleanly into one subject mesh
    bromesh::MeshData worldMesh;
    for (const Cand* c : selected) {
        const bromesh::MeshData& md = c->node->mesh();
        const bromath::Mat4& wm = c->node->worldMatrix();
        const uint32_t baseV = static_cast<uint32_t>(worldMesh.vertexCount());
        const size_t vc = md.vertexCount();
        for (size_t i = 0; i < vc; ++i) {
            const bromath::Vec3 wp = bromath::mtransformPoint(
                wm, {md.positions[i * 3], md.positions[i * 3 + 1], md.positions[i * 3 + 2]});
            worldMesh.positions.push_back(wp.x);
            worldMesh.positions.push_back(wp.y);
            worldMesh.positions.push_back(wp.z);
        }
        for (uint32_t idx : md.indices) worldMesh.indices.push_back(baseV + idx);
        if (res.subjectName.empty()) res.subjectName = c->name;
    }

    res = analyzeMeshForm(worldMesh, opts);
    if (selected.size() > 1) res.subjectName = !opts.focus.empty() ? opts.focus : "subject";
    else if (!selected.empty()) res.subjectName = selected[0]->name;

    // Rendered light-transport modeling over non-sky pixels
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    int w = 0, h = 0;
    scene::vk::InspectCamera cam{};
    if (graph.renderer().readInspectSnapshot(rgba, depth, w, h, cam) && !rgba.empty() && w > 0 && h > 0) {
        std::vector<double> lumas;
        std::vector<double> warms;
        lumas.reserve(static_cast<size_t>(w) * h / 4);
        warms.reserve(static_cast<size_t>(w) * h / 4);
        size_t hi = 0, mid = 0, core = 0;
        const int step = std::max(1, std::min(w, h) / 180);
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                if (!(depth[idx] > 1e-7f)) continue;
                const auto lab = color::srgbToOklab(rgba[idx * 4], rgba[idx * 4 + 1], rgba[idx * 4 + 2]);
                lumas.push_back(lab.L);
                warms.push_back(lab.a + lab.b);
                if (lab.L > 0.76) ++hi;
                else if (lab.L >= 0.36) ++mid;
                else ++core;
            }
        }
        const size_t n = lumas.size();
        if (n >= 16) {
            res.lighting.analyzed = true;
            res.lighting.highlightShare = static_cast<double>(hi) / n;
            res.lighting.halftoneShare = static_cast<double>(mid) / n;
            res.lighting.coreShadowShare = static_cast<double>(core) / n;

            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lumas[a] < lumas[b]; });
            const double lP05 = lumas[order[n * 5 / 100]];
            const double lP25 = lumas[order[n / 4]];
            const double lP95 = lumas[order[n * 95 / 100]];
            res.lighting.reflectedLightShare = std::clamp((lP05 - 0.04) / 0.16, 0.0, 1.0);
            if (lP25 < 0.05) res.lighting.reflectedLightShare *= 0.5;

            double hiWarm = 0.0, shWarm = 0.0;
            const size_t q = std::max<size_t>(1, n / 5);
            for (size_t i = 0; i < q; ++i) shWarm += warms[order[i]];
            for (size_t i = n - q; i < n; ++i) hiWarm += warms[order[i]];
            res.lighting.warmCoolLightShift = (hiWarm / q) - (shWarm / q);

            const double rangeScore = std::clamp((lP95 - lP05) / 0.65, 0.0, 1.0);
            const double tierBalance = (res.lighting.highlightShare > 0.01 ? 0.34 : 0.0) +
                                       (res.lighting.halftoneShare > 0.25 ? 0.33 : 0.15) +
                                       (res.lighting.coreShadowShare > 0.04 ? 0.33 : 0.0);
            res.lighting.valueTurnScore = std::clamp(0.55 * rangeScore + 0.45 * tierBalance, 0.0, 1.0);
        }
    }

    return res;
}

}  // namespace bro::eye
