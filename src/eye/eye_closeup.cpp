#include "eye/eye_closeup.h"
#include "eye/eye_draw_internal.h"

#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include <broimage/encode.h>
#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <vector>

namespace bro::eye {
namespace {

bromath::Vec3 mulPt(const bromath::Mat4& m, const bromath::Vec3& v) {
    const float* d = m.data;
    return {
        d[0] * v.x + d[4] * v.y + d[8]  * v.z + d[12],
        d[1] * v.x + d[5] * v.y + d[9]  * v.z + d[13],
        d[2] * v.x + d[6] * v.y + d[10] * v.z + d[14],
    };
}

bromath::AABB3 worldAabb(const bromath::AABB3& b, const bromath::Mat4& m) {
    bromath::AABB3 out = bromath::aempty3();
    for (int i = 0; i < 8; ++i) {
        out = bromath::aexpand(out, mulPt(m, {
            (i & 1) ? b.max.x : b.min.x,
            (i & 2) ? b.max.y : b.min.y,
            (i & 4) ? b.max.z : b.min.z,
        }));
    }
    return out;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool containsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

struct BoundsItem {
    std::string name;
    bromath::AABB3 box{};
    float radius = 0.0f;
};

struct ViewDirSpec {
    bromath::Vec3 dir;
    float aimBiasY;
};

ViewDirSpec angleDirSpec(const std::string& angle) {
    const std::string k = toLower(angle);
    if (k == "front") return {{0.0f, 0.14f, 1.0f}, 0.0f};
    if (k == "side") return {{1.0f, 0.14f, 0.0f}, 0.0f};
    if (k == "grazing") return {bromath::vnorm({0.85f, 0.08f, 0.52f}), -0.18f};
    if (k == "underside") return {bromath::vnorm({0.62f, -0.34f, 0.70f}), 0.15f};
    if (k == "top") return {bromath::vnorm({0.05f, 0.99f, 0.12f}), 0.0f};
    if (k == "rear") return {bromath::vnorm({-0.55f, 0.36f, -0.75f}), 0.0f};
    return {bromath::vnorm({0.68f, 0.42f, 0.72f}), 0.0f};
}

bool isGroundLikeName(const std::string& name) {
    const std::string l = toLower(name);
    return l.find("floor") != std::string::npos || l.find("ground") != std::string::npos ||
           l.find("terrain") != std::string::npos || l.find("plinth") != std::string::npos ||
           l.find("sky") != std::string::npos || l.find("bedplate") != std::string::npos;
}

}  // namespace

EyeCloseupResult captureCloseup(scene::SceneGraph& graph, const EyeCloseupOptions& opts) {
    EyeCloseupResult res;

    std::vector<BoundsItem> items;
    graph.root()->traverse([&](scene::SceneNode* node) {
        if (!node || !node->visible()) return;
        bromath::AABB3 lb = bromath::aempty3();
        if (node->type() == scene::SceneNode::Type::Mesh) {
            auto* mn = static_cast<scene::MeshNode*>(node);
            if (mn->mesh().empty()) return;
            lb = mn->localBounds();
        } else if (node->type() == scene::SceneNode::Type::InstancedMesh) {
            auto* in = static_cast<scene::InstancedMeshNode*>(node);
            if (in->mesh().empty()) return;
            lb = in->localBounds();
        } else {
            return;
        }
        const bromath::AABB3 wb = worldAabb(lb, node->worldMatrix());
        const bromath::Vec3 ext = bromath::aextent(wb);
        BoundsItem bi;
        bi.name = node->name().empty() ? ("node#" + std::to_string(node->id())) : node->name();
        bi.box = wb;
        bi.radius = 0.5f * bromath::vlen(ext);
        items.push_back(std::move(bi));
    });

    if (items.empty()) return res;

    std::vector<std::string> queries = opts.targets;
    if (queries.empty()) {
        for (const auto& it : items) {
            if (isGroundLikeName(it.name) || it.radius < 0.08f || it.radius > 3.2f) continue;
            queries.push_back(it.name);
            if (queries.size() >= 4) break;
        }
        if (queries.empty()) queries.push_back(items.front().name);
    }

    const std::vector<std::string> angles = opts.angles.empty()
        ? std::vector<std::string>{"threeQuarter", "grazing"}
        : opts.angles;

    struct ResolvedTarget {
        EyeCloseupTargetInfo info;
        bromath::AABB3 box;
    };
    std::vector<ResolvedTarget> targets;
    for (const auto& q : queries) {
        if (q.empty()) continue;
        bromath::AABB3 merged = bromath::aempty3();
        int count = 0;
        for (const auto& it : items) {
            if (containsCI(it.name, q)) {
                if (count == 0) merged = it.box;
                else merged = bromath::aexpand(bromath::aexpand(merged, it.box.min), it.box.max);
                ++count;
            }
        }
        if (count == 0) continue;
        const bromath::Vec3 c = bromath::acenter(merged);
        const bromath::Vec3 e = bromath::aextent(merged);
        ResolvedTarget rt;
        rt.info.query = q;
        rt.info.matchedNodes = count;
        rt.info.center[0] = c.x; rt.info.center[1] = c.y; rt.info.center[2] = c.z;
        rt.info.extent[0] = e.x; rt.info.extent[1] = e.y; rt.info.extent[2] = e.z;
        rt.info.minY = merged.min.y;
        rt.info.maxY = merged.max.y;
        rt.box = merged;
        targets.push_back(rt);
        res.targets.push_back(rt.info);
    }

    if (targets.empty()) return res;

    scene::CameraNode* savedCamNode = graph.activeCamera();
    const bromath::Mat4 origV = graph.viewMatrix();
    const bromath::Mat4 origP = graph.projectionMatrix();
    const bromath::Vec3 origEye = graph.cameraEye();
    const float origFov = graph.cameraFovY();
    const float origNear = graph.cameraNearZ();
    const float origFar = graph.cameraFarZ();
    const int cw = graph.canvasWidth(), ch = graph.canvasHeight();
    const float p00 = origP.at(0, 0), p11 = origP.at(1, 1);
    const float aspect = (cw > 0 && ch > 0) ? (static_cast<float>(cw) / ch)
                     : ((std::abs(p00) > 1e-6f && std::abs(p11) > 1e-6f) ? std::abs(p11 / p00) : 16.0f / 9.0f);
    const bromath::Vec3 origUp{origV.at(1, 0), origV.at(1, 1), origV.at(1, 2)};
    const bromath::Vec3 origFwd{-origV.at(2, 0), -origV.at(2, 1), -origV.at(2, 2)};

    const double margin = std::clamp(opts.margin, 1.1, 8.0);
    const int maxPanels = std::clamp(opts.maxPanels, 1, 24);
    const int maxDim = std::clamp(opts.maxPanelDim, 128, 1280);

    std::vector<EyeImage> panels;
    std::vector<std::string> labels;

    for (const auto& rt : targets) {
        const bromath::Vec3 ctr{
            static_cast<float>(rt.info.center[0]),
            static_cast<float>(rt.info.center[1]),
            static_cast<float>(rt.info.center[2]),
        };
        const bromath::Vec3 ext{
            static_cast<float>(rt.info.extent[0]),
            static_cast<float>(rt.info.extent[1]),
            static_cast<float>(rt.info.extent[2]),
        };
        const float rad = std::max(0.14f, 0.5f * bromath::vlen(ext));
        const float dist = static_cast<float>(std::clamp(static_cast<double>(rad) * margin, 0.35, 14.0));

        for (const auto& ang : angles) {
            if (static_cast<int>(panels.size()) >= maxPanels) break;
            const ViewDirSpec spec = angleDirSpec(ang);
            const bromath::Vec3 aim{ctr.x, ctr.y + spec.aimBiasY * ext.y, ctr.z};
            const bromath::Vec3 camPos = aim + spec.dir * dist;
            const bromath::Vec3 upVec = (std::abs(spec.dir.y) > 0.96f) ? bromath::Vec3{0.0f, 0.0f, -1.0f} : bromath::Vec3{0.0f, 1.0f, 0.0f};
            graph.setCamera(0.62f, aspect, 0.02f, std::max(40.0f, origFar), camPos, aim, upVec);
            graph.renderForRead();
            int w = 0, h = 0;
            const auto rgba = graph.readTonemapPixelsRGBA(w, h);
            if (rgba.empty() || w <= 0 || h <= 0) continue;
            EyeImage im = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, maxDim);
            panels.push_back(std::move(im));
            labels.push_back(rt.info.query + " | " + ang);
        }
        if (static_cast<int>(panels.size()) >= maxPanels) break;
    }

    if (savedCamNode) graph.setActiveCamera(savedCamNode);
    else graph.setCamera(origFov, aspect, origNear, origFar, origEye, origEye + origFwd * 5.0f, origUp);

    res.panelCount = static_cast<int>(panels.size());
    if (!panels.empty()) {
        const auto sheet = draw::assembleSheet(std::move(panels), labels, "grid", 1);
        res.visual = {sheet.width, sheet.height, std::move(sheet.rgba)};
        if (!opts.path.empty()) {
            std::error_code ec;
            const std::filesystem::path outPath(opts.path);
            if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path(), ec);
            if (broimage::encode_png_file(opts.path, res.visual.rgba.data(), res.visual.width, res.visual.height, 4)) {
                res.visualPath = opts.path;
            }
        }
    }

    return res;
}

}  // namespace bro::eye
