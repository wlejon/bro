#include "eye/eye_lookdev.h"
#include "eye/eye_draw_internal.h"

#include "scene/debug_views.h"
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

bromath::Vec3 angleDir(const std::string& angle) {
    const std::string k = toLower(angle);
    if (k == "front") return {0.0f, 0.14f, 1.0f};
    if (k == "side") return {1.0f, 0.14f, 0.0f};
    if (k == "top") return bromath::vnorm({0.05f, 0.99f, 0.12f});
    if (k == "rear") return bromath::vnorm({-0.55f, 0.36f, -0.75f});
    return bromath::vnorm({0.68f, 0.42f, 0.72f});
}

EyeImage makeSquintPanel(const EyeImage& beauty) {
    const int w = beauty.width, h = beauty.height;
    EyeImage out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 255)};
    std::vector<float> lum(static_cast<size_t>(w) * h, 0.0f);
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = beauty.rgba.data() + static_cast<size_t>(i) * 4;
        lum[i] = 0.2126f * (p[0] / 255.0f) + 0.7152f * (p[1] / 255.0f) + 0.0722f * (p[2] / 255.0f);
    }
    // 5x5 separable box blur to evaluate broad value/mass grouping ("artist squint")
    std::vector<float> tmp(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            int cnt = 0;
            for (int dx = -2; dx <= 2; ++dx) {
                const int xx = std::clamp(x + dx, 0, w - 1);
                acc += lum[static_cast<size_t>(y) * w + xx];
                ++cnt;
            }
            tmp[static_cast<size_t>(y) * w + x] = acc / cnt;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            int cnt = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                const int yy = std::clamp(y + dy, 0, h - 1);
                acc += tmp[static_cast<size_t>(yy) * w + x];
                ++cnt;
            }
            const float v = std::clamp(acc / cnt, 0.0f, 1.0f);
            const uint8_t u = static_cast<uint8_t>(std::lround(v * 255.0f));
            uint8_t* o = out.rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            o[0] = u; o[1] = u; o[2] = u; o[3] = 255;
        }
    }
    return out;
}

struct HotspotMetrics {
    double clippedWhiteFrac = 0.0;
    int hotspotCount = 0;
};

HotspotMetrics measureBloomHotspots(const EyeImage& pBeauty, const EyeImage& pAlbedo, const EyeImage& pRough) {
    HotspotMetrics hm;
    const int pw = pBeauty.width, ph = pBeauty.height;
    if (pw <= 0 || ph <= 0 || pAlbedo.width != pw || pAlbedo.height != ph || pRough.width != pw || pRough.height != ph) {
        return hm;
    }
    const size_t nPix = static_cast<size_t>(pw) * ph;
    std::vector<uint8_t> hotMask(nPix, 0);
    size_t fgCount = 0, clippedCount = 0;
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const size_t idx = static_cast<size_t>(y) * pw + x;
            const size_t o = idx * 4;
            if (pRough.rgba[o] < 6) continue;
            ++fgCount;
            const double bLum = (0.2126 * pBeauty.rgba[o] + 0.7152 * pBeauty.rgba[o + 1] + 0.0722 * pBeauty.rgba[o + 2]) / 255.0;
            const double aLum = (0.2126 * pAlbedo.rgba[o] + 0.7152 * pAlbedo.rgba[o + 1] + 0.0722 * pAlbedo.rgba[o + 2]) / 255.0;
            if (bLum > 0.96 && aLum < 0.78) {
                ++clippedCount;
                hotMask[idx] = 1;
            }
        }
    }
    hm.clippedWhiteFrac = fgCount > 0 ? static_cast<double>(clippedCount) / fgCount : 0.0;

    std::vector<uint8_t> visited(nPix, 0);
    std::vector<size_t> q;
    const int dx4[4] = {1, -1, 0, 0};
    const int dy4[4] = {0, 0, 1, -1};
    for (size_t i = 0; i < nPix; ++i) {
        if (!hotMask[i] || visited[i]) continue;
        visited[i] = 1;
        q.clear();
        q.push_back(i);
        size_t head = 0;
        while (head < q.size()) {
            const size_t cur = q[head++];
            const int cx = static_cast<int>(cur % pw);
            const int cy = static_cast<int>(cur / pw);
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= pw || ny < 0 || ny >= ph) continue;
                const size_t ni = static_cast<size_t>(ny) * pw + nx;
                if (hotMask[ni] && !visited[ni]) {
                    visited[ni] = 1;
                    q.push_back(ni);
                }
            }
        }
        if (q.size() >= 3) ++hm.hotspotCount;
    }
    return hm;
}

}  // namespace

EyeLookdevResult captureLookdev(scene::SceneGraph& graph, const EyeLookdevOptions& opts) {
    EyeLookdevResult res;

    struct NodeVis {
        scene::SceneNode* node = nullptr;
        std::string name;
        bromath::AABB3 box{};
        bool wasVisible = true;
    };
    std::vector<NodeVis> nodes;
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
        NodeVis nv;
        nv.node = node;
        nv.name = node->name();
        nv.box = worldAabb(lb, node->worldMatrix());
        nv.wasVisible = node->visible();
        nodes.push_back(std::move(nv));
    });

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
    const int savedDebug = graph.renderer().debugView();

    if (!opts.target.empty()) {
        bromath::AABB3 merged = bromath::aempty3();
        std::vector<size_t> matched;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const bool parentMatch = nodes[i].node->parent() && containsCI(nodes[i].node->parent()->name(), opts.target);
            if (containsCI(nodes[i].name, opts.target) || parentMatch) {
                if (matched.empty()) merged = nodes[i].box;
                else merged = bromath::aexpand(bromath::aexpand(merged, nodes[i].box.min), nodes[i].box.max);
                matched.push_back(i);
            }
        }
        res.matchedNodes = static_cast<int>(matched.size());
        if (matched.empty()) {
            res.ok = false;
            return res;
        }
        for (size_t idx : matched) {
            if (nodes[idx].node->type() != scene::SceneNode::Type::Mesh) continue;
            auto* mn = static_cast<scene::MeshNode*>(nodes[idx].node);
            if (mn->effectiveUnlit() || mn->emissive() > 0.5f) continue;
            const bool translucent = mn->color()[3] < 0.99f;
            if (translucent) continue;
            ++res.checkedMeshes;
            const bool hasAlbedo = mn->hasBaseColorTexture();
            const bool hasNormal = mn->hasNormalTexture();
            const bool hasMR = mn->hasMetallicRoughnessTexture();
            const bool casts = mn->castsShadow();
            if (!hasAlbedo) ++res.untexturedCount;
            if (!hasNormal) ++res.noNormalMapCount;
            if (!casts) ++res.noShadowSolidCount;
            if (!hasAlbedo || !hasNormal || !casts) {
                res.materialWarnings.push_back({nodes[idx].name, hasAlbedo, hasNormal, hasMR, casts, translucent});
            }
        }
        if (opts.isolate) {
            std::vector<bool> keep(nodes.size(), false);
            for (size_t idx : matched) keep[idx] = true;
            for (size_t i = 0; i < nodes.size(); ++i) nodes[i].node->setVisible(keep[i]);
        }
        const bromath::Vec3 ctr = bromath::acenter(merged);
        const bromath::Vec3 ext = bromath::aextent(merged);
        const float rad = std::max(0.06f, 0.5f * bromath::vlen(ext));
        const float dist = static_cast<float>(std::clamp(static_cast<double>(rad) * opts.margin, 0.16, 14.0));
        const float nearZ = std::min(0.02f, std::max(0.004f, dist * 0.05f));
        const bromath::Vec3 dir = angleDir(opts.angle);
        const bromath::Vec3 upVec = (std::abs(dir.y) > 0.96f) ? bromath::Vec3{0.0f, 0.0f, -1.0f} : bromath::Vec3{0.0f, 1.0f, 0.0f};
        graph.setCamera(0.62f, aspect, nearZ, std::max(40.0f, origFar), ctr + dir * dist, ctr, upVec);
    } else {
        res.matchedNodes = static_cast<int>(nodes.size());
        for (const auto& nv : nodes) {
            if (nv.node->type() != scene::SceneNode::Type::Mesh) continue;
            auto* mn = static_cast<scene::MeshNode*>(nv.node);
            if (mn->effectiveUnlit() || mn->emissive() > 0.5f) continue;
            const bool translucent = mn->color()[3] < 0.99f;
            if (translucent) continue;
            ++res.checkedMeshes;
            const bool hasAlbedo = mn->hasBaseColorTexture();
            const bool hasNormal = mn->hasNormalTexture();
            const bool hasMR = mn->hasMetallicRoughnessTexture();
            const bool casts = mn->castsShadow();
            if (!hasAlbedo) ++res.untexturedCount;
            if (!hasNormal) ++res.noNormalMapCount;
            if (!casts) ++res.noShadowSolidCount;
            if (!hasAlbedo || !hasNormal || !casts) {
                res.materialWarnings.push_back({nv.name, hasAlbedo, hasNormal, hasMR, casts, translucent});
            }
        }
    }

    const int sheetW = std::clamp(opts.sheetWidth, 256, 1280);
    const int sheetH = std::clamp(opts.sheetHeight, 256, 1280);
    const int panelCap = std::max(96, std::max((sheetW - 24) / 2, (sheetH - 48) / 2));

    auto captureView = [&](int debugId) -> EyeImage {
        graph.renderer().setDebugView(debugId);
        graph.renderForRead();
        int w = 0, h = 0;
        const auto rgba = graph.readTonemapPixelsRGBA(w, h);
        if (rgba.empty() || w <= 0 || h <= 0) return {};
        return draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, panelCap);
    };

    const int idAlbedo = scene::debug_views::find("albedo");
    const int idRough = scene::debug_views::find("roughness");

    EyeImage pBeauty = captureView(0);
    EyeImage pAlbedo = captureView(idAlbedo);
    EyeImage pRough = captureView(idRough);

    // Always restore state before stats & PNG encode
    graph.renderer().setDebugView(savedDebug);
    if (opts.isolate && !opts.target.empty()) {
        for (const auto& nv : nodes) nv.node->setVisible(nv.wasVisible);
    }
    if (!opts.target.empty()) {
        if (savedCamNode) graph.setActiveCamera(savedCamNode);
        else graph.setCamera(origFov, aspect, origNear, origFar, origEye, origEye + origFwd * 5.0f, origUp);
    }

    if (pBeauty.rgba.empty() || pAlbedo.rgba.empty() || pRough.rgba.empty()) {
        res.ok = false;
        return res;
    }
    EyeImage pSquint = makeSquintPanel(pBeauty);

    // Compute quantitative lookdev statistics over drawn surface pixels.
    // Built-in lighting clamps surface roughness >= 0.04 (>= 10/255), so empty background reads 0 in roughness view.
    const int pw = pRough.width, ph = pRough.height;
    std::vector<double> rVals, lVals, sVals;
    rVals.reserve(static_cast<size_t>(pw) * ph / 2);
    lVals.reserve(rVals.capacity());
    sVals.reserve(rVals.capacity());
    size_t edgeHits = 0, fgCount = 0;

    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const size_t o = (static_cast<size_t>(y) * pw + x) * 4;
            const uint8_t rb = pRough.rgba[o];
            if (rb < 6) continue;  // skip sky / clear background
            ++fgCount;
            const double r = std::pow(rb / 255.0, 2.2);
            rVals.push_back(r);

            const double lum = (0.2126 * pBeauty.rgba[o] + 0.7152 * pBeauty.rgba[o + 1] + 0.0722 * pBeauty.rgba[o + 2]) / 255.0;
            lVals.push_back(lum);

            const double ar = pAlbedo.rgba[o] / 255.0;
            const double ag = pAlbedo.rgba[o + 1] / 255.0;
            const double ab = pAlbedo.rgba[o + 2] / 255.0;
            const double mx = std::max({ar, ag, ab});
            const double mn = std::min({ar, ag, ab});
            sVals.push_back(mx > 1e-4 ? (mx - mn) / mx : 0.0);

            if (x + 1 < pw) {
                const uint8_t rbR = pRough.rgba[o + 4];
                if ((rbR < 6) || std::abs(static_cast<int>(pBeauty.rgba[o]) - static_cast<int>(pBeauty.rgba[o + 4])) > 26) {
                    ++edgeHits;
                }
            }
        }
    }

    if (!rVals.empty()) {
        double rSum = 0.0, lSum = 0.0, sSum = 0.0;
        size_t glossyCount = 0;
        for (size_t i = 0; i < rVals.size(); ++i) {
            rSum += rVals[i];
            lSum += lVals[i];
            sSum += sVals[i];
            if (rVals[i] < 0.28) ++glossyCount;
        }
        const double invN = 1.0 / static_cast<double>(rVals.size());
        res.roughnessMean = rSum * invN;
        res.valueMean = lSum * invN;
        res.albedoSaturationMean = sSum * invN;
        res.glossyFrac = glossyCount * invN;
        double rVar = 0.0, lVar = 0.0;
        for (size_t i = 0; i < rVals.size(); ++i) {
            const double dr = rVals[i] - res.roughnessMean;
            const double dl = lVals[i] - res.valueMean;
            rVar += dr * dr;
            lVar += dl * dl;
        }
        res.roughnessStd = std::sqrt(rVar * invN);
        res.valueStd = std::sqrt(lVar * invN);
        std::sort(lVals.begin(), lVals.end());
        res.valueContrast = lVals[static_cast<size_t>(0.90 * (lVals.size() - 1))] -
                            lVals[static_cast<size_t>(0.10 * (lVals.size() - 1))];
        res.silhouetteEdgeDensity = fgCount > 0 ? static_cast<double>(edgeHits) / fgCount : 0.0;
    }

    const HotspotMetrics hm = measureBloomHotspots(pBeauty, pAlbedo, pRough);
    res.clippedWhiteFrac = hm.clippedWhiteFrac;
    res.bloomHotspotCount = hm.hotspotCount;

    char roughPill[64];
    std::snprintf(roughPill, sizeof(roughPill), "rough=%.2f glossy=%.0f%%", res.roughnessMean, res.glossyFrac * 100.0);
    draw::drawText(pRough, 6, 6, roughPill, 1, 175, 240, 210, true);
    char valPill[64];
    std::snprintf(valPill, sizeof(valPill), "val=%.2f con=%.2f hot=%d", res.valueMean, res.valueContrast, res.bloomHotspotCount);
    draw::drawText(pSquint, 6, 6, valPill, 1, 240, 225, 175, true);

    std::vector<EyeImage> panels;
    panels.push_back(std::move(pBeauty));
    panels.push_back(std::move(pAlbedo));
    panels.push_back(std::move(pRough));
    panels.push_back(std::move(pSquint));
    const std::vector<std::string> labels = {"beauty", "albedo", "roughness", "squint"};
    const auto sheet = draw::assembleExactSheet(panels, labels, 2, 2, sheetW, sheetH, 1);
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
    return res;
}

EyeSweepResult captureStateSweep(
    scene::SceneGraph& graph, const EyeSweepOptions& opts,
    const std::vector<std::pair<std::string, std::function<void()>>>& states) {
    EyeSweepResult res;
    if (states.empty()) {
        res.ok = false;
        return res;
    }
    const size_t nStates = std::min<size_t>(states.size(), 9);
    const int cols = (nStates <= 3) ? static_cast<int>(nStates) : (nStates <= 4 ? 2 : 3);
    const int rows = static_cast<int>((nStates + cols - 1) / cols);
    const int sheetW = std::clamp(opts.sheetWidth > 0 ? opts.sheetWidth : 768, 256, 1280);
    const int defH = (nStates <= 3) ? 432 : 768;
    const int sheetH = std::clamp(opts.sheetHeight > 0 ? opts.sheetHeight : defH, 256, 1280);
    const int cellCap = std::max(96, std::max((sheetW - 8 * (cols + 1)) / cols,
                                              (sheetH - 8 * (rows + 1)) / rows));

    const int savedDebug = graph.renderer().debugView();
    const int idAlbedo = scene::debug_views::find("albedo");
    const int idRough = scene::debug_views::find("roughness");

    auto capDebug = [&](int debugId) -> EyeImage {
        graph.renderer().setDebugView(debugId);
        graph.renderForRead();
        int w = 0, h = 0;
        const auto rgba = graph.readTonemapPixelsRGBA(w, h);
        if (rgba.empty() || w <= 0 || h <= 0) return {};
        return draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, cellCap);
    };

    std::vector<EyeImage> panels;
    std::vector<std::string> labels;
    panels.reserve(nStates);
    labels.reserve(nStates);

    for (size_t i = 0; i < nStates; ++i) {
        if (states[i].second) states[i].second();
        EyeImage beauty = capDebug(0);
        EyeImage albedo = capDebug(idAlbedo);
        EyeImage rough = capDebug(idRough);
        if (beauty.rgba.empty() || albedo.rgba.empty() || rough.rgba.empty()) continue;

        EyeSweepStateResult sr;
        sr.label = states[i].first;
        std::vector<double> lVals;
        const int pw = beauty.width, ph = beauty.height;
        lVals.reserve(static_cast<size_t>(pw) * ph / 2);
        double lSum = 0.0;
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const size_t o = (static_cast<size_t>(y) * pw + x) * 4;
                if (rough.rgba[o] < 6) continue;
                const double lum = (0.2126 * beauty.rgba[o] + 0.7152 * beauty.rgba[o + 1] + 0.0722 * beauty.rgba[o + 2]) / 255.0;
                lVals.push_back(lum);
                lSum += lum;
            }
        }
        if (!lVals.empty()) {
            sr.valueMean = lSum / static_cast<double>(lVals.size());
            std::sort(lVals.begin(), lVals.end());
            sr.valueContrast = lVals[static_cast<size_t>(0.90 * (lVals.size() - 1))] -
                               lVals[static_cast<size_t>(0.10 * (lVals.size() - 1))];
        }
        const HotspotMetrics hm = measureBloomHotspots(beauty, albedo, rough);
        sr.clippedWhiteFrac = hm.clippedWhiteFrac;
        sr.hotspotCount = hm.hotspotCount;

        char pill[96];
        std::snprintf(pill, sizeof(pill), "val=%.2f con=%.2f hot=%d", sr.valueMean, sr.valueContrast, sr.hotspotCount);
        draw::drawText(beauty, 6, 6, pill, 1, 240, 225, 175, true);

        labels.push_back(sr.label);
        panels.push_back(std::move(beauty));
        res.states.push_back(std::move(sr));
    }

    graph.renderer().setDebugView(savedDebug);
    res.stateCount = static_cast<int>(res.states.size());
    if (panels.empty()) {
        res.ok = false;
        return res;
    }

    const auto sheet = draw::assembleExactSheet(panels, labels, cols, rows, sheetW, sheetH, 1);
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
    return res;
}

}  // namespace bro::eye
