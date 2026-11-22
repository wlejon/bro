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
#include <cstdio>
#include <filesystem>
#include <unordered_set>
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

bromath::Vec3 mulDir(const bromath::Mat4& m, const bromath::Vec3& v) {
    const float* d = m.data;
    return {
        d[0] * v.x + d[4] * v.y + d[8]  * v.z,
        d[1] * v.x + d[5] * v.y + d[9]  * v.z,
        d[2] * v.x + d[6] * v.y + d[10] * v.z,
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
    scene::SceneNode* node = nullptr;
    std::string name;
    bromath::AABB3 box{};
    bromath::Mat4 world{};
    float radius = 0.0f;
    bool wasVisible = true;
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

std::string stripDigitsSuffix(std::string s) {
    // Strip trailing numbers/underscores and also collapse embedded numbers like "Lily 4 Pivot" -> "Lily Pivot"
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (std::isdigit(ch) || ch == '_') continue;
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(static_cast<char>(ch));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string prototypeStem(const BoundsItem& it) {
    if (it.node && it.node->parent() && !it.node->parent()->name().empty()) {
        const std::string p = stripDigitsSuffix(it.node->parent()->name());
        const std::string pl = toLower(p);
        if (!p.empty() && pl != "root" && pl != "__root") {
            const size_t pv = pl.rfind(" pivot");
            if (pv != std::string::npos && pv + 6 == pl.size()) return p.substr(0, pv);
            return p;
        }
    }
    std::string stem = stripDigitsSuffix(it.name);
    for (const char* suf : {" wing l", " wing r", " body", " lotus", " pad", " wood", " canopy", " stem", " pivot"}) {
        const std::string low = toLower(stem);
        const std::string sfx(suf);
        if (low.size() > sfx.size() && low.compare(low.size() - sfx.size(), sfx.size(), sfx) == 0) {
            stem = stem.substr(0, stem.size() - sfx.size());
            break;
        }
    }
    stem = stripDigitsSuffix(stem);
    return stem.empty() ? it.name : stem;
}

void drawArrow2D(EyeImage& im, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
    draw::drawLine(im, x0, y0, x1, y1, 2, r, g, b, 240);
    const double dx = static_cast<double>(x1 - x0);
    const double dy = static_cast<double>(y1 - y0);
    const double len = std::hypot(dx, dy);
    if (len < 6.0) return;
    const double ux = dx / len, uy = dy / len;
    const double px = -uy, py = ux;
    const int ax = static_cast<int>(std::lround(x1 - ux * 6.0 + px * 3.5));
    const int ay = static_cast<int>(std::lround(y1 - uy * 6.0 + py * 3.5));
    const int bx = static_cast<int>(std::lround(x1 - ux * 6.0 - px * 3.5));
    const int by = static_cast<int>(std::lround(y1 - uy * 6.0 - py * 3.5));
    draw::drawLine(im, x1, y1, ax, ay, 2, r, g, b, 240);
    draw::drawLine(im, x1, y1, bx, by, 2, r, g, b, 240);
}

void formatExtentBadge(const bromath::Vec3& ext, char* buf, size_t n) {
    const float maxE = std::max({ext.x, ext.y, ext.z});
    if (maxE < 0.99f) {
        std::snprintf(buf, n, "%.0fx%.0fx%.0fcm", ext.x * 100.0f, ext.y * 100.0f, ext.z * 100.0f);
    } else {
        std::snprintf(buf, n, "%.2fx%.2fx%.2fm", ext.x, ext.y, ext.z);
    }
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
        const bromath::Mat4 wm = node->worldMatrix();
        const bromath::AABB3 wb = worldAabb(lb, wm);
        const bromath::Vec3 ext = bromath::aextent(wb);
        BoundsItem bi;
        bi.node = node;
        bi.name = node->name().empty() ? ("node#" + std::to_string(node->id())) : node->name();
        bi.box = wb;
        bi.world = wm;
        bi.radius = 0.5f * bromath::vlen(ext);
        bi.wasVisible = node->visible();
        items.push_back(std::move(bi));
    });

    if (items.empty()) return res;

    const std::vector<std::string> angles = opts.angles.empty()
        ? std::vector<std::string>{"threeQuarter", "grazing"}
        : opts.angles;
    const int maxPanels = std::clamp(opts.maxPanels, 1, 24);
    const int maxTargets = std::max(1, maxPanels / static_cast<int>(angles.size()));

    std::vector<std::string> queries = opts.targets;
    if (queries.empty() && toLower(opts.mode) == "inventory") {
        // Group into distinct prototype classes, sorting smallest-to-largest so tiny props (dragonflies/leaves)
        // appear alongside focal props rather than only the largest terrain objects.
        std::vector<BoundsItem> candidates;
        for (const auto& it : items) {
            if (isGroundLikeName(it.name) || it.radius < 0.015f) continue;
            candidates.push_back(it);
        }
        std::sort(candidates.begin(), candidates.end(), [](const BoundsItem& a, const BoundsItem& b) {
            return a.radius < b.radius;
        });
        std::unordered_set<std::string> seen;
        std::vector<std::string> stems;
        for (const auto& c : candidates) {
            const std::string s = prototypeStem(c);
            const std::string sl = toLower(s);
            if (seen.insert(sl).second) stems.push_back(s);
        }
        // Interleave small and large prototypes when more prototypes exist than slots
        size_t lo = 0, hi = stems.size() ? stems.size() - 1 : 0;
        bool takeSmall = true;
        while (lo <= hi && static_cast<int>(queries.size()) < maxTargets && !stems.empty()) {
            queries.push_back(takeSmall ? stems[lo++] : stems[hi--]);
            takeSmall = !takeSmall;
        }
    } else if (queries.empty()) {
        for (const auto& it : items) {
            if (isGroundLikeName(it.name) || it.radius < 0.08f || it.radius > 3.2f) continue;
            queries.push_back(it.name);
            if (static_cast<int>(queries.size()) >= maxTargets) break;
        }
        if (queries.empty()) queries.push_back(items.front().name);
    }

    struct ResolvedTarget {
        EyeCloseupTargetInfo info;
        bromath::AABB3 box;
        std::vector<size_t> itemIndices;
    };
    std::vector<ResolvedTarget> targets;
    for (const auto& q : queries) {
        if (q.empty()) continue;
        bromath::AABB3 merged = bromath::aempty3();
        std::vector<size_t> matched;
        for (size_t idx = 0; idx < items.size(); ++idx) {
            const auto& it = items[idx];
            const bool parentMatch = it.node && it.node->parent() && containsCI(it.node->parent()->name(), q);
            if (containsCI(it.name, q) || parentMatch) {
                if (matched.empty()) merged = it.box;
                else merged = bromath::aexpand(bromath::aexpand(merged, it.box.min), it.box.max);
                matched.push_back(idx);
            }
        }
        if (matched.empty()) continue;
        // If inventory stem matched e.g. 5 copies spread across the scene, frame the first instance cleanly.
        if (toLower(opts.mode) == "inventory" && matched.size() > 1) {
            const auto& firstIt = items[matched.front()];
            const bool sharedParent = firstIt.node && firstIt.node->parent() && containsCI(firstIt.node->parent()->name(), q);
            if (!sharedParent) {
                merged = firstIt.box;
                matched.resize(1);
            }
        }
        const bromath::Vec3 c = bromath::acenter(merged);
        const bromath::Vec3 e = bromath::aextent(merged);
        ResolvedTarget rt;
        rt.info.query = q;
        rt.info.matchedNodes = static_cast<int>(matched.size());
        rt.info.center[0] = c.x; rt.info.center[1] = c.y; rt.info.center[2] = c.z;
        rt.info.extent[0] = e.x; rt.info.extent[1] = e.y; rt.info.extent[2] = e.z;
        rt.info.minY = merged.min.y;
        rt.info.maxY = merged.max.y;
        rt.box = merged;
        rt.itemIndices = std::move(matched);
        targets.push_back(std::move(rt));
        res.targets.push_back(targets.back().info);
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
    const bool useExact = (opts.sheetWidth > 0 && opts.sheetHeight > 0);
    const int maxDim = std::clamp(opts.maxPanelDim, 96, 1280);

    std::vector<EyeImage> panels;
    std::vector<std::string> labels;

    const int plannedPanels = std::min(maxPanels, static_cast<int>(targets.size() * angles.size()));
    const int cols = plannedPanels <= 2 ? std::max(1, plannedPanels)
                   : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(plannedPanels))));
    const int rows = std::max(1, (plannedPanels + cols - 1) / cols);
    const int panelCap = useExact
        ? std::max(64, std::max((opts.sheetWidth - 8 * (cols + 1)) / cols,
                                (opts.sheetHeight - 8 * (rows + 1)) / rows))
        : maxDim;

    for (const auto& rt : targets) {
        if (opts.isolate) {
            std::unordered_set<size_t> keep(rt.itemIndices.begin(), rt.itemIndices.end());
            for (size_t i = 0; i < items.size(); ++i) {
                items[i].node->setVisible(keep.count(i) > 0);
            }
        }

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
        const float rad = std::max(0.06f, 0.5f * bromath::vlen(ext));
        const float dist = static_cast<float>(std::clamp(static_cast<double>(rad) * margin, 0.16, 14.0));
        const float nearZ = std::min(0.02f, std::max(0.004f, dist * 0.05f));

        for (const auto& ang : angles) {
            if (static_cast<int>(panels.size()) >= maxPanels) break;
            const ViewDirSpec spec = angleDirSpec(ang);
            const bromath::Vec3 aim{ctr.x, ctr.y + spec.aimBiasY * ext.y, ctr.z};
            const bromath::Vec3 camPos = aim + spec.dir * dist;
            const bromath::Vec3 upVec = (std::abs(spec.dir.y) > 0.96f) ? bromath::Vec3{0.0f, 0.0f, -1.0f} : bromath::Vec3{0.0f, 1.0f, 0.0f};
            graph.setCamera(0.62f, aspect, nearZ, std::max(40.0f, origFar), camPos, aim, upVec);
            graph.renderForRead();
            int w = 0, h = 0;
            const auto rgba = graph.readTonemapPixelsRGBA(w, h);
            if (rgba.empty() || w <= 0 || h <= 0) continue;
            EyeImage im = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, panelCap);

            if (opts.showScale) {
                char badge[64];
                formatExtentBadge(ext, badge, sizeof(badge));
                draw::drawText(im, 6, im.height - 14, badge, 1, 185, 240, 215, true);
            }
            if (opts.showHeading && !rt.itemIndices.empty()) {
                const bromath::Mat4& wm = items[rt.itemIndices.front()].world;
                const bromath::Vec3 xDir = bromath::vnorm(mulDir(wm, {1.0f, 0.0f, 0.0f}));
                const bromath::Vec3 zDir = bromath::vnorm(mulDir(wm, {0.0f, 0.0f, 1.0f}));
                const float axLen = std::max(0.06f, rad * 0.75f);
                const float cwf = static_cast<float>(cw > 0 ? cw : w);
                const float chf = static_cast<float>(ch > 0 ? ch : h);
                auto toPx = [&](const bromath::Vec3& p, int& ox, int& oy) -> bool {
                    float lx = 0, ly = 0, d = 0;
                    if (!graph.projectLocal(p, lx, ly, d) || d <= 0.0f) return false;
                    ox = static_cast<int>(std::lround((lx / cwf) * im.width));
                    oy = static_cast<int>(std::lround((ly / chf) * im.height));
                    return true;
                };
                int cx = 0, cy = 0, xx = 0, xy = 0, zx = 0, zy = 0;
                if (toPx(ctr, cx, cy)) {
                    if (toPx(ctr + xDir * axLen, xx, xy)) drawArrow2D(im, cx, cy, xx, xy, 70, 225, 255);
                    if (toPx(ctr + zDir * axLen, zx, zy)) drawArrow2D(im, cx, cy, zx, zy, 255, 195, 70);
                }
            }

            panels.push_back(std::move(im));
            labels.push_back(rt.info.query + " | " + ang);
        }
        if (static_cast<int>(panels.size()) >= maxPanels) break;
    }

    if (opts.isolate) {
        for (const auto& it : items) it.node->setVisible(it.wasVisible);
    }
    if (savedCamNode) graph.setActiveCamera(savedCamNode);
    else graph.setCamera(origFov, aspect, origNear, origFar, origEye, origEye + origFwd * 5.0f, origUp);

    res.panelCount = static_cast<int>(panels.size());
    if (!panels.empty()) {
        const auto sheet = useExact
            ? draw::assembleExactSheet(panels, labels, cols, rows, opts.sheetWidth, opts.sheetHeight, 1)
            : draw::assembleSheet(panels, labels, "grid", 1);
        res.width = sheet.width;
        res.height = sheet.height;
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
