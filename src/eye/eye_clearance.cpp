#include "eye/eye_clearance.h"
#include "eye/eye_draw_internal.h"

#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include <broimage/encode.h>
#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>

namespace bro::eye {
namespace {

struct MeshRef {
    scene::MeshNode* node = nullptr;
    scene::SceneNode* parent = nullptr;
    std::string name;
    bromath::Mat4 mat0{};
};

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
        const bromath::Vec3 c{
            (i & 1) ? b.max.x : b.min.x,
            (i & 2) ? b.max.y : b.min.y,
            (i & 4) ? b.max.z : b.min.z,
        };
        out = bromath::aexpand(out, mulPt(m, c));
    }
    return out;
}

bool aabbOverlap(const bromath::AABB3& a, const bromath::AABB3& b, double eps, bromath::AABB3& outBox, double& outDepth) {
    const double ox = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    const double oy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    const double oz = std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z);
    outDepth = std::min({ox, oy, oz});
    if (outDepth <= eps) return false;
    outBox.min = {std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)};
    outBox.max = {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)};
    return true;
}

bool segTriIntersect(const bromath::Vec3& p0, const bromath::Vec3& p1,
                     const bromath::Vec3& t0, const bromath::Vec3& t1, const bromath::Vec3& t2,
                     double epsT, bromath::Vec3& outPt) {
    const bromath::Vec3 d = p1 - p0;
    const bromath::Vec3 e1 = t1 - t0;
    const bromath::Vec3 e2 = t2 - t0;
    const bromath::Vec3 h = bromath::vcross(d, e2);
    const float a = bromath::vdot(e1, h);
    if (std::abs(a) < 1e-7f) return false;
    const float f = 1.0f / a;
    const bromath::Vec3 s = p0 - t0;
    const float u = f * bromath::vdot(s, h);
    if (u < 0.01f || u > 0.99f) return false;
    const bromath::Vec3 q = bromath::vcross(s, e1);
    const float v = f * bromath::vdot(d, q);
    if (v < 0.01f || u + v > 0.99f) return false;
    const float t = f * bromath::vdot(e2, q);
    if (t <= static_cast<float>(epsT) || t >= static_cast<float>(1.0 - epsT)) return false;
    outPt = p0 + d * t;
    return true;
}

// Strict volumetric triangle-triangle piercing test (ignores coplanar abutting faces).
bool triTriPierce(const bromath::Vec3 a[3], const bromath::Vec3 b[3], double eps, bromath::Vec3& outMid) {
    const float minAx = std::min({a[0].x, a[1].x, a[2].x});
    const float maxAx = std::max({a[0].x, a[1].x, a[2].x});
    const float minBx = std::min({b[0].x, b[1].x, b[2].x});
    const float maxBx = std::max({b[0].x, b[1].x, b[2].x});
    if (maxAx < minBx || maxBx < minAx) return false;
    const float minAy = std::min({a[0].y, a[1].y, a[2].y});
    const float maxAy = std::max({a[0].y, a[1].y, a[2].y});
    const float minBy = std::min({b[0].y, b[1].y, b[2].y});
    const float maxBy = std::max({b[0].y, b[1].y, b[2].y});
    if (maxAy < minBy || maxBy < minAy) return false;
    const float minAz = std::min({a[0].z, a[1].z, a[2].z});
    const float maxAz = std::max({a[0].z, a[1].z, a[2].z});
    const float minBz = std::min({b[0].z, b[1].z, b[2].z});
    const float maxBz = std::max({b[0].z, b[1].z, b[2].z});
    if (maxAz < minBz || maxBz < minAz) return false;

    const float edgeTol = static_cast<float>(std::clamp(eps * 2.0, 0.002, 0.08));
    bromath::Vec3 sum{0, 0, 0};
    int hits = 0;
    bromath::Vec3 pt{};
    for (int e = 0; e < 3; ++e) {
        if (segTriIntersect(a[e], a[(e + 1) % 3], b[0], b[1], b[2], edgeTol, pt)) {
            sum = sum + pt;
            ++hits;
        }
        if (segTriIntersect(b[e], b[(e + 1) % 3], a[0], a[1], a[2], edgeTol, pt)) {
            sum = sum + pt;
            ++hits;
        }
    }
    if (hits == 0) return false;
    outMid = sum * (1.0f / static_cast<float>(hits));
    return true;
}

bool pairIgnored(const std::string& a, const std::string& b, const EyeClearanceOptions& opts) {
    for (const auto& [pa, pb] : opts.ignorePairs) {
        if ((pa == a && pb == b) || (pa == b && pb == a)) return true;
    }
    return false;
}

bool matricesDiffer(const bromath::Mat4& a, const bromath::Mat4& b, float tol = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (std::abs(a.data[i] - b.data[i]) > tol) return true;
    }
    return false;
}

struct WorldTri {
    bromath::Vec3 v[3];
    bromath::AABB3 box;
};

std::vector<WorldTri> extractTrisInBox(const scene::MeshNode& mn, const bromath::Mat4& m, const bromath::AABB3& clip) {
    std::vector<WorldTri> out;
    const auto& md = mn.mesh();
    const size_t nIdx = md.indices.size();
    out.reserve(std::min<size_t>(nIdx / 3, 256));
    for (size_t i = 0; i + 2 < nIdx; i += 3) {
        const uint32_t i0 = md.indices[i + 0];
        const uint32_t i1 = md.indices[i + 1];
        const uint32_t i2 = md.indices[i + 2];
        if (static_cast<size_t>(i0) * 3 + 2 >= md.positions.size() ||
            static_cast<size_t>(i1) * 3 + 2 >= md.positions.size() ||
            static_cast<size_t>(i2) * 3 + 2 >= md.positions.size()) continue;
        WorldTri t;
        t.v[0] = mulPt(m, {md.positions[i0 * 3 + 0], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]});
        t.v[1] = mulPt(m, {md.positions[i1 * 3 + 0], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]});
        t.v[2] = mulPt(m, {md.positions[i2 * 3 + 0], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]});
        t.box = bromath::aempty3();
        t.box = bromath::aexpand(bromath::aexpand(bromath::aexpand(t.box, t.v[0]), t.v[1]), t.v[2]);
        if (t.box.max.x < clip.min.x || t.box.min.x > clip.max.x) continue;
        if (t.box.max.y < clip.min.y || t.box.min.y > clip.max.y) continue;
        if (t.box.max.z < clip.min.z || t.box.min.z > clip.max.z) continue;
        out.push_back(t);
        if (out.size() >= 1500) break;  // bound worst-case check time cleanly
    }
    return out;
}

}  // namespace

EyeClearanceResult analyzeClearance(scene::SceneGraph& graph,
                                    const EyeClearanceOptions& opts,
                                    const std::function<void(int, int)>& stepFn) {
    EyeClearanceResult res;
    const int steps = std::clamp(opts.steps, 1, 180);
    const double eps = std::clamp(opts.eps, 1e-5, 0.5);

    std::vector<MeshRef> meshes;
    graph.root()->traverse([&](scene::SceneNode* node) {
        if (!node || !node->visible()) return;
        if (node->type() == scene::SceneNode::Type::Mesh) {
            auto* mn = static_cast<scene::MeshNode*>(node);
            const auto& md = mn->mesh();
            if (md.positions.empty() || md.indices.empty()) return;
            meshes.push_back({mn, node->parent(), node->name().empty() ? ("mesh#" + std::to_string(node->id())) : node->name(), node->worldMatrix()});
        }
    });

    const size_t nM = meshes.size();
    res.posesChecked = steps;
    res.totalMeshPairs = static_cast<int>((nM * (nM > 0 ? nM - 1 : 0)) / 2);

    // Track worst collision per (i, j) pair across all poses
    std::map<std::pair<size_t, size_t>, EyeClearancePair> pairMap;
    std::map<std::pair<size_t, size_t>, bromath::Mat4> relMat0;
    std::vector<int> poseDynamicHits(static_cast<size_t>(steps), 0);

    for (size_t i = 0; i < nM; ++i) {
        const bromath::Mat4 invA = bromath::minverse(meshes[i].mat0);
        for (size_t j = i + 1; j < nM; ++j) {
            relMat0[{i, j}] = bromath::mmul(invA, meshes[j].mat0);
        }
    }

    for (int k = 0; k < steps; ++k) {
        if (stepFn) stepFn(k, steps);

        std::vector<bromath::Mat4> mats(nM);
        std::vector<bromath::AABB3> boxes(nM);
        for (size_t i = 0; i < nM; ++i) {
            mats[i] = meshes[i].node->worldMatrix();
            boxes[i] = worldAabb(meshes[i].node->localBounds(), mats[i]);
        }

        for (size_t i = 0; i < nM; ++i) {
            for (size_t j = i + 1; j < nM; ++j) {
                if (opts.ignoreSameParent && meshes[i].parent && meshes[i].parent == meshes[j].parent) continue;
                if (pairIgnored(meshes[i].name, meshes[j].name, opts)) continue;

                bromath::AABB3 ovBox{};
                double ovDepth = 0.0;
                if (!aabbOverlap(boxes[i], boxes[j], eps, ovBox, ovDepth)) continue;

                const bromath::Mat4 relK = bromath::mmul(bromath::minverse(mats[i]), mats[j]);
                const bool relMoved = (steps > 1) && matricesDiffer(relK, relMat0[{i, j}]);
                if (steps > 1 && opts.ignoreStatic && !relMoved) {
                    // Check later only if pair moved on another step
                    const bool movedAbs = matricesDiffer(mats[i], meshes[i].mat0) || matricesDiffer(mats[j], meshes[j].mat0);
                    if (!movedAbs) continue;
                }

                const auto trisA = extractTrisInBox(*meshes[i].node, mats[i], ovBox);
                if (trisA.empty()) continue;
                const auto trisB = extractTrisInBox(*meshes[j].node, mats[j], ovBox);
                if (trisB.empty()) continue;

                int hitCount = 0;
                bromath::Vec3 cSum{0, 0, 0};
                bromath::Vec3 mid{};
                for (const auto& ta : trisA) {
                    for (const auto& tb : trisB) {
                        if (triTriPierce(ta.v, tb.v, eps, mid)) {
                            cSum = cSum + mid;
                            ++hitCount;
                            if (hitCount >= 64) break;
                        }
                    }
                    if (hitCount >= 64) break;
                }

                if (hitCount > 0) {
                    const bromath::Vec3 c = cSum * (1.0f / static_cast<float>(hitCount));
                    auto key = std::make_pair(i, j);
                    auto it = pairMap.find(key);
                    if (it == pairMap.end() || hitCount > it->second.hitTriangles) {
                        EyeClearancePair ep;
                        ep.nodeA = meshes[i].name;
                        ep.nodeB = meshes[j].name;
                        ep.hitTriangles = hitCount;
                        ep.poseIndex = k;
                        ep.centroid[0] = c.x;
                        ep.centroid[1] = c.y;
                        ep.centroid[2] = c.z;
                        ep.overlapDepth = ovDepth;
                        ep.dynamic = relMoved;
                        pairMap[key] = std::move(ep);
                    } else if (relMoved) {
                        it->second.dynamic = true;
                    }
                    if (relMoved || steps == 1) ++poseDynamicHits[static_cast<size_t>(k)];
                }
            }
        }
    }

    int bestPose = 0, bestHits = -1;
    for (int k = 0; k < steps; ++k) {
        if (poseDynamicHits[static_cast<size_t>(k)] > bestHits) {
            bestHits = poseDynamicHits[static_cast<size_t>(k)];
            bestPose = k;
        }
    }
    res.worstPoseIndex = bestPose;

    const bool filterStatic = opts.ignoreStatic && (steps > 1);
    for (auto& [key, ep] : pairMap) {
        if (ep.dynamic) ++res.dynamicCollisionCount;
        else ++res.staticCollisionCount;
        if (filterStatic && !ep.dynamic) continue;
        res.collisions.push_back(std::move(ep));
    }
    std::sort(res.collisions.begin(), res.collisions.end(), [](const EyeClearancePair& a, const EyeClearancePair& b) {
        if (a.dynamic != b.dynamic) return a.dynamic > b.dynamic;
        return a.hitTriangles > b.hitTriangles;
    });

    res.ok = filterStatic ? (res.dynamicCollisionCount == 0) : res.collisions.empty();

    if (!opts.path.empty()) {
        if (stepFn) stepFn(res.worstPoseIndex, steps);
        graph.renderForRead();
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = graph.readTonemapPixelsRGBA(w, h);
        if (!rgba.empty() && w > 0 && h > 0) {
            EyeImage img = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, opts.maxPanelDim);
            // Blueprint monochrome tint so red/amber collision markers pop immediately
            for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; ++i) {
                uint8_t* p = &img.rgba[i * 4];
                const double luma = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                p[0] = static_cast<uint8_t>(std::clamp(luma * 0.42 + 8.0, 0.0, 255.0));
                p[1] = static_cast<uint8_t>(std::clamp(luma * 0.56 + 14.0, 0.0, 255.0));
                p[2] = static_cast<uint8_t>(std::clamp(luma * 0.78 + 26.0, 0.0, 255.0));
                p[3] = 255;
            }
            const float cw = static_cast<float>(graph.canvasWidth() > 0 ? graph.canvasWidth() : w);
            const float ch = static_cast<float>(graph.canvasHeight() > 0 ? graph.canvasHeight() : h);
            for (size_t ci = 0; ci < std::min<size_t>(res.collisions.size(), 18); ++ci) {
                const auto& c = res.collisions[ci];
                float lx = 0.0f, ly = 0.0f, depth = 0.0f;
                if (!graph.projectLocal({static_cast<float>(c.centroid[0]), static_cast<float>(c.centroid[1]), static_cast<float>(c.centroid[2])},
                                        lx, ly, depth) || depth <= 0.0f) {
                    continue;
                }
                const int px = std::clamp(static_cast<int>(std::lround((lx / cw) * img.width)), 6, img.width - 7);
                const int py = std::clamp(static_cast<int>(std::lround((ly / ch) * img.height)), 6, img.height - 7);
                const uint8_t cr = c.dynamic ? 255 : 255;
                const uint8_t cg = c.dynamic ? 68 : 185;
                const uint8_t cb = c.dynamic ? 82 : 60;
                draw::drawCircle(img, px, py, 9, 2, cr, cg, cb, 245);
                draw::drawLine(img, px - 12, py, px + 12, py, 1, cr, cg, cb, 220);
                draw::drawLine(img, px, py - 12, px, py + 12, 1, cr, cg, cb, 220);
                char tag[96];
                std::snprintf(tag, sizeof(tag), "%s x %s (%dt@p%d)", c.nodeA.c_str(), c.nodeB.c_str(), c.hitTriangles, c.poseIndex);
                draw::drawText(img, std::clamp(px + 10, 4, std::max(4, img.width - 210)),
                               std::clamp(py - 8, 4, std::max(4, img.height - 14)), tag, 1, cr, cg, cb, true);
            }
            char banner[96];
            if (res.ok) {
                std::snprintf(banner, sizeof(banner), "CLEARANCE OK | %d poses | %d pairs | 0 dynamic hits",
                              res.posesChecked, res.totalMeshPairs);
                draw::drawText(img, 12, 12, banner, 1, 110, 245, 165, true);
            } else {
                std::snprintf(banner, sizeof(banner), "COLLISIONS: %d dyn / %d stat @ pose %d",
                              res.dynamicCollisionCount, res.staticCollisionCount, res.worstPoseIndex);
                draw::drawText(img, 12, 12, banner, 1, 255, 95, 105, true);
            }
            res.visual = std::move(img);
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
