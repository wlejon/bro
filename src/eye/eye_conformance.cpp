#include "eye/eye_conformance.h"
#include "eye/eye_draw_internal.h"

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
#include <limits>
#include <vector>

namespace bro::eye {
namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool containsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

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

}  // namespace

EyeConformanceResult auditConformance(scene::SceneGraph& graph, const EyeConformanceOptions& opts) {
    EyeConformanceResult res;
    std::vector<scene::MeshNode*> allMeshes;
    graph.root()->traverse([&](scene::SceneNode* node) {
        if (!node || !node->visible() || node->type() != scene::SceneNode::Type::Mesh) return;
        auto* mn = static_cast<scene::MeshNode*>(node);
        if (!mn->mesh().empty()) allMeshes.push_back(mn);
    });

    struct VisDot {
        bromath::Vec3 pt{};
        bool good = true;
    };
    std::vector<VisDot> dots;
    bromath::AABB3 focusBox = bromath::aempty3();
    bool hasFocus = false;

    for (const auto& spec : opts.pairs) {
        EyeConformancePairResult pr;
        pr.subject = spec.subject;

        std::vector<scene::MeshNode*> subjs, surfs;
        for (scene::MeshNode* mn : allMeshes) {
            if (containsCI(mn->name(), spec.subject)) subjs.push_back(mn);
            for (const std::string& sf : spec.surfaces) {
                if (containsCI(mn->name(), sf)) {
                    surfs.push_back(mn);
                    break;
                }
            }
        }
        pr.surfaceCount = static_cast<int>(surfs.size());
        if (subjs.empty() || surfs.empty()) {
            pr.ok = false;
            res.ok = false;
            res.pairs.push_back(std::move(pr));
            continue;
        }

        struct SurfInv {
            scene::MeshNode* node = nullptr;
            bromath::Mat4 world{};
            bromath::Mat4 invWorld{};
        };
        std::vector<SurfInv> surfData;
        surfData.reserve(surfs.size());
        for (scene::MeshNode* sf : surfs) {
            const bromath::Mat4 wm = sf->worldMatrix();
            surfData.push_back({sf, wm, bromath::minverse(wm)});
        }

        std::vector<double> gaps;
        double worstAbs = -1.0;
        bromath::Vec3 worstPt{};
        const int budget = std::max(8, spec.maxSamples);

        size_t totalSubVerts = 0;
        for (scene::MeshNode* sb : subjs) totalSubVerts += sb->mesh().vertexCount();
        const size_t stride = std::max<size_t>(1, totalSubVerts / static_cast<size_t>(budget));

        for (scene::MeshNode* sb : subjs) {
            const auto& md = sb->mesh();
            const bromath::Mat4 sbW = sb->worldMatrix();
            const size_t vc = md.vertexCount();
            for (size_t vi = 0; vi < vc; vi += stride) {
                const bromath::Vec3 wp = mulPt(sbW, {md.positions[vi * 3], md.positions[vi * 3 + 1], md.positions[vi * 3 + 2]});
                focusBox = hasFocus ? bromath::aexpand(focusBox, wp) : bromath::AABB3{wp, wp};
                hasFocus = true;

                double bestAbs = std::numeric_limits<double>::infinity();
                double bestSigned = 0.0;
                for (const auto& sf : surfData) {
                    const bromath::Vec3 lp = mulPt(sf.invWorld, wp);
                    const float q[3] = {lp.x, lp.y, lp.z};
                    const auto hit = sf.node->bvh().closestPoint(sf.node->mesh(), q);
                    if (!hit.hit) continue;
                    const bromath::Vec3 hw = mulPt(sf.world, {hit.position[0], hit.position[1], hit.position[2]});
                    const bromath::Vec3 hn = bromath::vnorm(mulDir(sf.world, {hit.normal[0], hit.normal[1], hit.normal[2]}));
                    const bromath::Vec3 delta = wp - hw;
                    const double dist = bromath::vlen(delta);
                    if (dist < bestAbs) {
                        bestAbs = dist;
                        bestSigned = (bromath::vdot(delta, hn) >= 0.0f) ? dist : -dist;
                    }
                }
                if (std::isfinite(bestAbs)) {
                    gaps.push_back(bestSigned);
                    if (bestAbs > worstAbs) {
                        worstAbs = bestAbs;
                        worstPt = wp;
                    }
                    dots.push_back({wp, bestSigned <= spec.maxGap && bestSigned >= spec.minGap});
                }
            }
        }

        pr.sampleCount = static_cast<int>(gaps.size());
        if (!gaps.empty()) {
            size_t airCnt = 0, penCnt = 0;
            for (double g : gaps) {
                if (g > spec.maxGap) ++airCnt;
                if (g < spec.minGap) ++penCnt;
            }
            std::sort(gaps.begin(), gaps.end());
            pr.minGap = gaps.front();
            pr.p50Gap = gaps[gaps.size() / 2];
            pr.p95Gap = gaps[static_cast<size_t>(0.95 * (gaps.size() - 1))];
            pr.maxGap = gaps.back();
            pr.airGapFrac = static_cast<double>(airCnt) / gaps.size();
            pr.penetratingFrac = static_cast<double>(penCnt) / gaps.size();
            pr.worstPoint[0] = worstPt.x;
            pr.worstPoint[1] = worstPt.y;
            pr.worstPoint[2] = worstPt.z;
            pr.ok = (pr.p95Gap <= spec.maxGap) && (pr.airGapFrac < 0.12);
        } else {
            pr.ok = false;
        }
        if (!pr.ok) res.ok = false;
        res.pairs.push_back(std::move(pr));
    }

    if (!opts.path.empty() && hasFocus) {
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

        const bromath::Vec3 ctr = bromath::acenter(focusBox);
        const bromath::Vec3 ext = bromath::aextent(focusBox);
        const float rad = std::max(0.15f, 0.5f * bromath::vlen(ext));
        const float dist = std::clamp(rad * 2.4f, 0.35f, 16.0f);
        const bromath::Vec3 dirs[2] = {bromath::vnorm({0.65f, 0.30f, 0.70f}), bromath::vnorm({-0.85f, 0.18f, 0.50f})};
        std::vector<EyeImage> panels;
        std::vector<std::string> labels = {"threeQuarter", "side"};

        for (int vi = 0; vi < 2; ++vi) {
            graph.setCamera(0.62f, aspect, 0.01f, std::max(40.0f, origFar), ctr + dirs[vi] * dist, ctr, {0, 1, 0});
            graph.renderForRead();
            int w = 0, h = 0;
            auto rgba = graph.readTonemapPixelsRGBA(w, h);
            if (!rgba.empty() && w > 0 && h > 0) {
                EyeImage img{w, h, std::move(rgba)};
                for (const auto& dt : dots) {
                    float lx = 0, ly = 0, d = 0;
                    if (graph.projectLocal(dt.pt, lx, ly, d) && d > 0.0f) {
                        const int px = static_cast<int>(std::lround(lx));
                        const int py = static_cast<int>(std::lround(ly));
                        draw::drawCircle(img, px, py, 3, 2, dt.good ? 90 : 255, dt.good ? 235 : 70, dt.good ? 140 : 70, 255);
                    }
                }
                panels.push_back(std::move(img));
            }
        }

        if (savedCamNode) graph.setActiveCamera(savedCamNode);
        else graph.setCamera(origFov, aspect, origNear, origFar, origEye, origEye + origFwd * 5.0f, origUp);

        if (!panels.empty()) {
            const int sheetW = std::clamp(opts.sheetWidth > 0 ? opts.sheetWidth : 768, 256, 1280);
            const int sheetH = std::clamp(opts.sheetHeight > 0 ? opts.sheetHeight : 384, 192, 1280);
            const auto sheet = draw::assembleExactSheet(panels, labels, 2, 1, sheetW, sheetH, 1);
            res.width = sheet.width;
            res.height = sheet.height;
            res.visual = {sheet.width, sheet.height, std::move(sheet.rgba)};
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
