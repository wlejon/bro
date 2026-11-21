#include "eye/eye_scene.h"
#include "eye/eye_color.h"
#include "eye/eye_measure.h"

#include "scene/nodes/camera_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_view_inspect.h"

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace bro::eye {
namespace {

struct MeshEntry {
    scene::MeshNode* node = nullptr;
    bromath::AABB3 worldBox;
    std::string name;
    bool likelyGround = false;
};

double unprojectViewDist(const scene::vk::InspectCamera& cam, int x, int y, int w, int h, float d) {
    const double cx = 2.0 * (x + 0.5) / w - 1.0;
    const double cy = 2.0 * (y + 0.5) / h - 1.0;
    const bromath::Mat4& ip = cam.invProj;
    double v[4];
    for (int r = 0; r < 4; ++r) {
        v[r] = ip.at(r, 0) * cx + ip.at(r, 1) * cy + ip.at(r, 2) * d + ip.at(r, 3);
    }
    if (std::abs(v[3]) < 1e-9) return 0.0;
    const double vx = v[0] / v[3];
    const double vy = v[1] / v[3];
    const double vz = v[2] / v[3];
    return std::sqrt(vx * vx + vy * vy + vz * vz);
}

bool projectPoint(const scene::vk::InspectCamera& cam, const bromath::Vec3& worldPos, int w, int h,
                  float& outPx, float& outPy, float& outNdcZ) {
    const double rx = static_cast<double>(worldPos.x) - cam.eye[0];
    const double ry = static_cast<double>(worldPos.y) - cam.eye[1];
    const double rz = static_cast<double>(worldPos.z) - cam.eye[2];
    const bromath::Mat4& m = cam.relViewProj;
    const double cx = m.at(0, 0) * rx + m.at(0, 1) * ry + m.at(0, 2) * rz + m.at(0, 3);
    const double cy = m.at(1, 0) * rx + m.at(1, 1) * ry + m.at(1, 2) * rz + m.at(1, 3);
    const double cz = m.at(2, 0) * rx + m.at(2, 1) * ry + m.at(2, 2) * rz + m.at(2, 3);
    const double cw = m.at(3, 0) * rx + m.at(3, 1) * ry + m.at(3, 2) * rz + m.at(3, 3);
    if (!std::isfinite(cw) || cw <= 1e-6) return false;
    const double nx = cx / cw;
    const double ny = cy / cw;
    const double nz = cz / cw;
    // Vulkan clip space: y down (-1 top .. +1 bottom), x (-1..+1), reversed-Z (1 near .. 0 far).
    outPx = static_cast<float>((nx * 0.5 + 0.5) * w);
    outPy = static_cast<float>((ny * 0.5 + 0.5) * h);
    outNdcZ = static_cast<float>(nz);
    return true;
}

std::string nodeDisplayName(const scene::SceneNode* n) {
    if (!n) return "";
    if (!n->name().empty()) return n->name();
    return "node#" + std::to_string(n->id());
}

// Compute triangle-level screen mask for a MeshNode tested against the depth buffer,
// and measure Oklab ΔE along its screen silhouette boundary.
void computeNodeScreenMask(const MeshEntry& entry, const scene::vk::InspectCamera& cam,
                           const std::vector<uint8_t>& rgba, const std::vector<float>& depth,
                           int w, int h, EyeSceneNodeInfo& info) {
    const bromesh::MeshData& md = entry.node->mesh();
    if (md.empty() || md.indices.size() < 3) return;

    const bromath::Mat4& worldM = entry.node->worldMatrix();
    const size_t vc = md.vertexCount();
    const size_t tc = md.triangleCount();

    std::vector<float> px(vc), py(vc), pz(vc);
    std::vector<uint8_t> pok(vc, 0);
    for (size_t i = 0; i < vc; ++i) {
        const bromath::Vec3 lp{md.positions[i * 3], md.positions[i * 3 + 1], md.positions[i * 3 + 2]};
        const bromath::Vec3 wp = bromath::mtransformPoint(worldM, lp);
        if (projectPoint(cam, wp, w, h, px[i], py[i], pz[i])) {
            pok[i] = 1;
        }
    }

    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    int minX = w, minY = h, maxX = -1, maxY = -1;
    int64_t potentialCount = 0;
    int64_t visibleCount = 0;
    double minDist = std::numeric_limits<double>::infinity();
    double maxDist = 0.0;

    // Rasterize up to 8000 triangles cleanly in bounded time.
    const size_t triStep = std::max<size_t>(1, tc / 8000);
    for (size_t t = 0; t < tc; t += triStep) {
        const uint32_t i0 = md.indices[t * 3 + 0];
        const uint32_t i1 = md.indices[t * 3 + 1];
        const uint32_t i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        if (!pok[i0] || !pok[i1] || !pok[i2]) continue;

        const float x0 = px[i0], y0 = py[i0], z0 = pz[i0];
        const float x1 = px[i1], y1 = py[i1], z1 = pz[i1];
        const float x2 = px[i2], y2 = py[i2], z2 = pz[i2];

        const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
        if (std::abs(area) < 0.25f) continue;

        const int bx0 = std::clamp(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, w - 1);
        const int bx1 = std::clamp(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, w - 1);
        const int by0 = std::clamp(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, h - 1);
        const int by1 = std::clamp(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, h - 1);

        const int sxStep = std::max(1, (bx1 - bx0) / 128);
        const int syStep = std::max(1, (by1 - by0) / 128);

        for (int yy = by0; yy <= by1; yy += syStep) {
            for (int xx = bx0; xx <= bx1; xx += sxStep) {
                const float fx = xx + 0.5f;
                const float fy = yy + 0.5f;
                const float w0 = ((x1 - fx) * (y2 - fy) - (y1 - fy) * (x2 - fx)) / area;
                const float w1 = ((x2 - fx) * (y0 - fy) - (y2 - fy) * (x0 - fx)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) continue;

                const float z = w0 * z0 + w1 * z1 + w2 * z2;
                if (z <= 0.0f) continue;

                const size_t idx = static_cast<size_t>(yy) * w + xx;
                if (mask[idx] == 0) {
                    ++potentialCount;
                    const float bufZ = depth[idx];
                    // Reversed-Z: larger = nearer. Allow small depth epsilon for vertex quantization.
                    if (z >= bufZ * 0.96f - 1e-4f) {
                        mask[idx] = 1;
                        ++visibleCount;
                        minX = std::min(minX, xx);
                        minY = std::min(minY, yy);
                        maxX = std::max(maxX, xx);
                        maxY = std::max(maxY, yy);
                        const double dist = unprojectViewDist(cam, xx, yy, w, h, bufZ > 1e-7f ? bufZ : z);
                        if (dist > 0.0) {
                            minDist = std::min(minDist, dist);
                            maxDist = std::max(maxDist, dist);
                        }
                    } else {
                        mask[idx] = 2;  // occluded
                    }
                }
            }
        }
    }

    const double totalPx = static_cast<double>(w) * h;
    info.screenCoverage = visibleCount / std::max(1.0, totalPx);
    info.visibleFraction = potentialCount > 0 ? static_cast<double>(visibleCount) / static_cast<double>(potentialCount) : 0.0;
    if (maxX >= minX && maxY >= minY) {
        info.bounds2d = {
            static_cast<double>(minX),
            static_cast<double>(minY),
            static_cast<double>(maxX - minX + 1),
            static_cast<double>(maxY - minY + 1),
        };
    }
    if (std::isfinite(minDist)) info.minDepth = minDist;
    info.maxDepth = maxDist;

    if (visibleCount > 4) {
        info.frameClipped = (minX <= 1 || minY <= 1 || maxX >= w - 2 || maxY >= h - 2);

        uint64_t lHist[256]{};
        double sumL = 0.0, sumL2 = 0.0;
        int64_t lCnt = 0;
        for (int y = std::max(0, minY); y <= std::min(h - 1, maxY); ++y) {
            for (int x = std::max(0, minX); x <= std::min(w - 1, maxX); ++x) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                if (mask[idx] != 1) continue;
                const uint8_t* p = &rgba[idx * 4];
                const double L = color::srgbToOklab(p[0], p[1], p[2]).L;
                sumL += L;
                sumL2 += L * L;
                const int bin = std::clamp(static_cast<int>(L * 255.0 + 0.5), 0, 255);
                ++lHist[bin];
                ++lCnt;
            }
        }
        if (lCnt > 0) {
            info.lumaMean = sumL / static_cast<double>(lCnt);
            info.lumaStd = std::sqrt(std::max(0.0, sumL2 / static_cast<double>(lCnt) - info.lumaMean * info.lumaMean));
            auto qL = [&](double q) {
                const uint64_t tgt = static_cast<uint64_t>(std::ceil(q * lCnt));
                uint64_t acc = 0;
                for (int b = 0; b < 256; ++b) {
                    acc += lHist[b];
                    if (acc >= tgt) return static_cast<double>(b) / 255.0;
                }
                return 1.0;
            };
            info.formRange = std::max(0.0, qL(0.95) - qL(0.05));
        }

        double deSum = 0.0;
        int64_t edgeCnt = 0;
        for (int y = std::max(2, minY); y <= std::min(h - 3, maxY); ++y) {
            for (int x = std::max(2, minX); x <= std::min(w - 3, maxX); ++x) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                if (mask[idx] != 1) continue;
                // Only test true interior boundary pixels adjacent to mask!=1, sampling 2px outside
                // to skip 1px MSAA/FXAA edge blending.
                const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& d : dirs) {
                    const size_t adj = static_cast<size_t>(y + d[1]) * w + (x + d[0]);
                    if (mask[adj] == 1) continue;
                    const size_t inIdx = static_cast<size_t>(y - d[1]) * w + (x - d[0]);
                    const size_t outIdx = static_cast<size_t>(y + d[1] * 2) * w + (x + d[0] * 2);
                    const size_t sA = (mask[inIdx] == 1) ? inIdx : idx;
                    const size_t sB = (mask[outIdx] != 1) ? outIdx : adj;
                    const uint8_t* pA = &rgba[sA * 4];
                    const uint8_t* pB = &rgba[sB * 4];
                    const double de = color::oklabDeltaE(
                        color::srgbToOklab(pA[0], pA[1], pA[2]),
                        color::srgbToOklab(pB[0], pB[1], pB[2]));
                    deSum += de;
                    ++edgeCnt;
                    break;
                }
            }
        }
        if (edgeCnt > 0) info.silhouetteContrast = deSum / static_cast<double>(edgeCnt);
    }
}

// Check if two meshes actually interpenetrate beyond their axis-aligned world bounds.
double checkMeshInterpenetration(const MeshEntry& a, const MeshEntry& b, double eps) {
    const bromesh::MeshData& ma = a.node->mesh();
    const bromesh::MeshData& mb = b.node->mesh();
    if (ma.empty() || mb.empty()) return 0.0;

    // Conservative interior AABB overlap check first.
    const double ox = std::min(a.worldBox.max.x, b.worldBox.max.x) - std::max(a.worldBox.min.x, b.worldBox.min.x);
    const double oy = std::min(a.worldBox.max.y, b.worldBox.max.y) - std::max(a.worldBox.min.y, b.worldBox.min.y);
    const double oz = std::min(a.worldBox.max.z, b.worldBox.max.z) - std::max(a.worldBox.min.z, b.worldBox.min.z);
    const double minOverlap = std::min({ox, oy, oz});
    if (minOverlap <= eps) return 0.0;

    // Verify using triangle query / vertex containment with shrunk AABB.
    // Query triangles of `a` inside shrunk intersection box against `b`'s BVH.
    const bromath::Mat4 invB = bromath::minverse(b.node->worldMatrix());
    const bromath::Mat4 invA = bromath::minverse(a.node->worldMatrix());

    const auto& bvhB = b.node->bvh();
    const auto& bvhA = a.node->bvh();

    auto isPointInsideMesh = [&](const bromath::Vec3& lp, const bromath::AABB3& localBox,
                                 const bromesh::MeshData& m, const bromesh::MeshBVH& bvh) -> bool {
        if (lp.x <= localBox.min.x + eps || lp.x >= localBox.max.x - eps ||
            lp.y <= localBox.min.y + eps || lp.y >= localBox.max.y - eps ||
            lp.z <= localBox.min.z + eps || lp.z >= localBox.max.z - eps) {
            return false;
        }
        if (bvh.empty()) return true;
        const float q[3] = {lp.x, lp.y, lp.z};
        const auto hit = bvh.closestPoint(m, q);
        if (!hit.hit || hit.distance <= eps) return false;
        const float dx = lp.x - hit.position[0];
        const float dy = lp.y - hit.position[1];
        const float dz = lp.z - hit.position[2];
        const float dotN = dx * hit.normal[0] + dy * hit.normal[1] + dz * hit.normal[2];
        return dotN < -static_cast<float>(eps) * 0.5f;
    };

    auto anyVertexDeepInsideOther = [&](const bromesh::MeshData& srcM, const bromath::Mat4& srcWorld,
                                        const scene::MeshNode* dstNode, const bromath::Mat4& dstInv,
                                        const bromesh::MeshData& dstM, const bromesh::MeshBVH& dstBvh) -> bool {
        const auto& dstLocalBox = dstNode->localBounds();
        const size_t vc = srcM.vertexCount();
        const size_t step = std::max<size_t>(1, vc / 128);
        for (size_t i = 0; i < vc; i += step) {
            const bromath::Vec3 wp = bromath::mtransformPoint(srcWorld,
                {srcM.positions[i * 3], srcM.positions[i * 3 + 1], srcM.positions[i * 3 + 2]});
            if (!bromath::acontains(b.worldBox, wp) && !bromath::acontains(a.worldBox, wp)) continue;
            const bromath::Vec3 lp = bromath::mtransformPoint(dstInv, wp);
            if (isPointInsideMesh(lp, dstLocalBox, dstM, dstBvh)) return true;
        }
        return false;
    };

    if (anyVertexDeepInsideOther(ma, a.node->worldMatrix(), b.node, invB, mb, bvhB) ||
        anyVertexDeepInsideOther(mb, b.node->worldMatrix(), a.node, invA, ma, bvhA)) {
        return minOverlap;
    }

    // Center-of-overlap BVH signed-depth probe for intersecting thin boxes (e.g. crossed slabs).
    const bromath::Vec3 mid{
        0.5f * (std::max(a.worldBox.min.x, b.worldBox.min.x) + std::min(a.worldBox.max.x, b.worldBox.max.x)),
        0.5f * (std::max(a.worldBox.min.y, b.worldBox.min.y) + std::min(a.worldBox.max.y, b.worldBox.max.y)),
        0.5f * (std::max(a.worldBox.min.z, b.worldBox.min.z) + std::min(a.worldBox.max.z, b.worldBox.max.z)),
    };
    const bromath::Vec3 lpa = bromath::mtransformPoint(invA, mid);
    const bromath::Vec3 lpb = bromath::mtransformPoint(invB, mid);
    if (isPointInsideMesh(lpa, a.node->localBounds(), ma, bvhA) &&
        isPointInsideMesh(lpb, b.node->localBounds(), mb, bvhB)) {
        return minOverlap;
    }
    return 0.0;
}

}  // namespace

EyeSceneResult auditScene(scene::SceneGraph& graph, const EyeSceneOptions& opts) {
    EyeSceneResult res;
    if (opts.render) {
        graph.renderForRead();
    }

    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    scene::vk::InspectCamera cam{};
    int w = 0, h = 0;
    if (!graph.renderer().readInspectSnapshot(rgba, depth, w, h, cam) || rgba.empty() || depth.empty()) {
        res.ok = false;
        res.score = 0.0;
        EyeIssue iss;
        iss.kind = "no-render";
        iss.severity = "error";
        iss.message = "Scene has no rendered GPU snapshot";
        res.issues.push_back(std::move(iss));
        res.errorCount = 1;
        return res;
    }

    res.width = w;
    res.height = h;
    res.frame = measureImage({rgba.data(), w, h, w * 4});

    const auto& cs = graph.cullStats();
    res.exposureEv = cs.exposureEv;
    res.meteredEv = cs.meteredEv;
    res.meshDrawn = cs.meshDrawn;
    res.meshCulled = cs.meshCulled;

    // 1. Depth & sky statistics
    std::vector<double> dists;
    const int step = std::max(1, std::min(w, h) / 160);
    int64_t sampled = 0, skyCount = 0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            ++sampled;
            const float d = depth[static_cast<size_t>(y) * w + x];
            if (!(d > 1e-7f)) {
                ++skyCount;
                continue;
            }
            const double dist = unprojectViewDist(cam, x, y, w, h, d);
            if (dist > 0.0 && std::isfinite(dist)) dists.push_back(dist);
        }
    }
    res.skyRatio = sampled > 0 ? static_cast<double>(skyCount) / static_cast<double>(sampled) : 1.0;
    if (!dists.empty()) {
        std::sort(dists.begin(), dists.end());
        res.nearDist = dists[dists.size() * 5 / 100];
        res.medianDist = dists[dists.size() / 2];
        res.farDist = dists[dists.size() * 95 / 100];
    }

    // 2. Collect visible Triangle MeshNodes
    std::vector<MeshEntry> meshes;
    double minSceneY = std::numeric_limits<double>::infinity();
    double maxFootprint = 0.0;
    graph.root()->traverse([&](scene::SceneNode* n) {
        if (!n || !n->renderVisible() || n->type() != scene::SceneNode::Type::Mesh) return;
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (!mn->hasDrawableMesh() || mn->drawMode() != scene::MeshNode::DrawMode::Triangles) return;
        const bromath::AABB3 wb = bromath::atransform(mn->localBounds(), mn->worldMatrix());
        if (bromath::aisEmpty(wb)) return;
        MeshEntry entry;
        entry.node = mn;
        entry.worldBox = wb;
        entry.name = nodeDisplayName(mn);
        minSceneY = std::min(minSceneY, static_cast<double>(wb.min.y));
        const double xzSpan = std::max(wb.max.x - wb.min.x, wb.max.z - wb.min.z);
        maxFootprint = std::max(maxFootprint, xzSpan);
        meshes.push_back(std::move(entry));
    });

    for (auto& e : meshes) {
        const double xzSpan = std::max(e.worldBox.max.x - e.worldBox.min.x, e.worldBox.max.z - e.worldBox.min.z);
        const double yThick = e.worldBox.max.y - e.worldBox.min.y;
        const std::string lower = e.name;
        const bool namedGround = (lower == "ground" || lower.find("ground_") != std::string::npos ||
                                  lower.find("_ground") != std::string::npos ||
                                  lower.find("floor") != std::string::npos ||
                                  lower.find("terrain") != std::string::npos);
        e.likelyGround = namedGround || (xzSpan > 0.6 * maxFootprint && yThick < 0.25 * xzSpan && e.worldBox.min.y <= minSceneY + 0.05);
    }

    // 3. Per-node screen coverage, silhouette contrast, inverted normals & UV stretch
    int focusIdx = -1;
    double bestFocusCov = -1.0;
    std::vector<size_t> focusMatches;

    res.nodes.reserve(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& e = meshes[i];
        EyeSceneNodeInfo ni;
        ni.id = e.node->id();
        ni.name = e.name;
        ni.visible = true;
        computeNodeScreenMask(e, cam, rgba, depth, w, h, ni);

        if (!opts.focus.empty()) {
            if (e.name == opts.focus || (!e.likelyGround && e.name.find(opts.focus) != std::string::npos)) {
                focusMatches.push_back(i);
                if (ni.screenCoverage > bestFocusCov || e.name == opts.focus) {
                    bestFocusCov = (e.name == opts.focus) ? 2.0 : ni.screenCoverage;
                    focusIdx = static_cast<int>(i);
                }
            }
        } else if (!e.likelyGround && ni.screenCoverage > bestFocusCov) {
            bestFocusCov = ni.screenCoverage;
            focusIdx = static_cast<int>(i);
        }

        // Geometry sanity: inverted normals & UV stretch
        const auto& md = e.node->mesh();
        const size_t tc = md.triangleCount();
        if (tc > 0 && md.hasNormals()) {
            size_t badNormals = 0;
            const size_t tStep = std::max<size_t>(1, tc / 1024);
            size_t checkedTris = 0;
            for (size_t t = 0; t < tc; t += tStep) {
                const uint32_t i0 = md.indices[t * 3 + 0];
                const uint32_t i1 = md.indices[t * 3 + 1];
                const uint32_t i2 = md.indices[t * 3 + 2];
                const bromath::Vec3 p0{md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]};
                const bromath::Vec3 p1{md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]};
                const bromath::Vec3 p2{md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]};
                const bromath::Vec3 fn = bromath::vcross(p1 - p0, p2 - p0);
                if (bromath::vlen(fn) < 1e-8f) continue;
                const bromath::Vec3 vn{
                    md.normals[i0 * 3] + md.normals[i1 * 3] + md.normals[i2 * 3],
                    md.normals[i0 * 3 + 1] + md.normals[i1 * 3 + 1] + md.normals[i2 * 3 + 1],
                    md.normals[i0 * 3 + 2] + md.normals[i1 * 3 + 2] + md.normals[i2 * 3 + 2],
                };
                if (bromath::vdot(fn, vn) < 0.0f) ++badNormals;
                ++checkedTris;
            }
            if (checkedTris > 0 && static_cast<double>(badNormals) / checkedTris > 0.35) {
                EyeIssue iss;
                iss.kind = "inverted-normals";
                iss.severity = "warning";
                iss.selector = e.name;
                iss.nodeName = e.name;
                iss.hasSpatial = true;
                iss.rect = ni.bounds2d;
                iss.value = static_cast<double>(badNormals) / checkedTris;
                iss.message = "Mesh " + e.name + " has inverted face/vertex normals";
                res.issues.push_back(std::move(iss));
            }
        }

        if (tc > 0 && md.hasUVs() && opts.uvStretchThreshold > 1.0) {
            std::vector<double> ratios;
            const size_t tStep = std::max<size_t>(1, tc / 1024);
            for (size_t t = 0; t < tc; t += tStep) {
                const uint32_t i0 = md.indices[t * 3 + 0];
                const uint32_t i1 = md.indices[t * 3 + 1];
                const uint32_t i2 = md.indices[t * 3 + 2];
                const bromath::Vec3 p0{md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]};
                const bromath::Vec3 p1{md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]};
                const bromath::Vec3 p2{md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]};
                const double a3 = 0.5 * bromath::vlen(bromath::vcross(p1 - p0, p2 - p0));
                const double u0 = md.uvs[i0 * 2], v0 = md.uvs[i0 * 2 + 1];
                const double u1 = md.uvs[i1 * 2], v1 = md.uvs[i1 * 2 + 1];
                const double u2 = md.uvs[i2 * 2], v2 = md.uvs[i2 * 2 + 1];
                const double a2 = 0.5 * std::abs((u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0));
                if (a3 > 1e-6 && a2 > 1e-8) ratios.push_back(a3 / a2);
            }
            if (ratios.size() >= 4) {
                std::sort(ratios.begin(), ratios.end());
                const double med = std::max(1e-9, ratios[ratios.size() / 2]);
                const double p95 = ratios[ratios.size() * 95 / 100];
                const double spread = p95 / med;
                if (spread > opts.uvStretchThreshold) {
                    EyeIssue iss;
                    iss.kind = "uv-stretch";
                    iss.severity = "warning";
                    iss.selector = e.name;
                    iss.nodeName = e.name;
                    iss.hasSpatial = true;
                    iss.value = spread;
                    iss.rect = ni.bounds2d;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "High UV texel-density stretch on %s (%.1fx spread)", e.name.c_str(), spread);
                    iss.message = buf;
                    res.issues.push_back(std::move(iss));
                }
            }
        }

        res.nodes.push_back(std::move(ni));
    }

    if (focusIdx >= 0 && focusIdx < static_cast<int>(res.nodes.size())) {
        const auto& fn = res.nodes[focusIdx];
        if (fn.screenCoverage > 0.002 && fn.silhouetteContrast < opts.minSilhouetteContrast) {
            EyeIssue iss;
            iss.kind = "low-silhouette-contrast";
            iss.severity = "warning";
            iss.selector = fn.name;
            iss.nodeName = fn.name;
            iss.hasSpatial = true;
            iss.value = fn.silhouetteContrast;
            iss.rect = fn.bounds2d;
            char buf[180];
            std::snprintf(buf, sizeof(buf),
                          "Weak figure-ground silhouette contrast on %s (deltaE %.1f < %.1f)",
                          fn.name.c_str(), fn.silhouetteContrast, opts.minSilhouetteContrast);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    if (opts.checkFraming && !opts.focus.empty() && !focusMatches.empty()) {
        double sumCov = 0.0, sumForm = 0.0;
        double ux0 = w, uy0 = h, ux1 = 0.0, uy1 = 0.0;
        bool clipped = false;
        for (size_t idx : focusMatches) {
            const auto& fn = res.nodes[idx];
            sumCov += fn.screenCoverage;
            sumForm += fn.formRange * fn.screenCoverage;
            if (fn.frameClipped) clipped = true;
            if (fn.bounds2d.w > 0 && fn.bounds2d.h > 0) {
                ux0 = std::min(ux0, fn.bounds2d.x);
                uy0 = std::min(uy0, fn.bounds2d.y);
                ux1 = std::max(ux1, fn.bounds2d.x + fn.bounds2d.w);
                uy1 = std::max(uy1, fn.bounds2d.y + fn.bounds2d.h);
            }
        }
        const EyeRect unionRect = (ux1 > ux0 && uy1 > uy0) ? EyeRect{ux0, uy0, ux1 - ux0, uy1 - uy0} : res.nodes[focusMatches[0]].bounds2d;
        const std::string& focusLabel = opts.focus;
        if (clipped) {
            std::string edges;
            if (ux0 <= 1.5) edges += "left,";
            if (uy0 <= 1.5) edges += "top,";
            if (ux1 >= w - 1.5) edges += "right,";
            if (uy1 >= h - 1.5) edges += "bottom,";
            if (!edges.empty()) edges.pop_back();
            EyeIssue iss;
            iss.kind = "frame-clipped";
            iss.severity = "warning";
            iss.selector = focusLabel;
            iss.nodeName = focusLabel;
            iss.hasSpatial = true;
            iss.rect = unionRect;
            iss.value = sumCov;
            iss.message = "Focus target '" + focusLabel + "' is clipped at viewport edge (" + edges + ")";
            res.issues.push_back(std::move(iss));
        }
        if (sumCov > 0.0 && sumCov < 0.03) {
            EyeIssue iss;
            iss.kind = "under-framed";
            iss.severity = "warning";
            iss.selector = focusLabel;
            iss.nodeName = focusLabel;
            iss.hasSpatial = true;
            iss.rect = unionRect;
            iss.value = sumCov;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Focus target '%s' fills only %.1f%% of frame (< 3%%)",
                          focusLabel.c_str(), sumCov * 100.0);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
        const double meanForm = (sumCov > 1e-6) ? (sumForm / sumCov) : 0.0;
        if (sumCov >= 0.01 && meanForm < 0.14) {
            EyeIssue iss;
            iss.kind = "flat-lighting";
            iss.severity = "warning";
            iss.selector = focusLabel;
            iss.nodeName = focusLabel;
            iss.hasSpatial = true;
            iss.rect = unionRect;
            iss.value = meanForm;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Flat lighting / weak form modeling on '%s' (L range %.2f < 0.14)",
                          focusLabel.c_str(), meanForm);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    if (opts.checkFraming && (res.frame.crushedBlackRatio > 0.22 || res.frame.blownWhiteRatio > 0.08)) {
        EyeIssue iss;
        iss.kind = "exposure-clip";
        iss.severity = "warning";
        iss.hasSpatial = true;
        iss.value = std::max(res.frame.crushedBlackRatio, res.frame.blownWhiteRatio);
        iss.rect = {0, 0, static_cast<double>(w), static_cast<double>(h)};
        char buf[180];
        std::snprintf(buf, sizeof(buf),
                      "Scene frame exposure clipping (%.1f%% crushed black, %.1f%% blown white)",
                      res.frame.crushedBlackRatio * 100.0, res.frame.blownWhiteRatio * 100.0);
        iss.message = buf;
        res.issues.push_back(std::move(iss));
    }

    // 4. Floating detection
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& a = meshes[i];
        if (a.likelyGround) continue;
        double bestGap = std::numeric_limits<double>::infinity();
        std::string supportName;
        for (size_t j = 0; j < meshes.size(); ++j) {
            if (i == j) continue;
            const auto& b = meshes[j];
            if (a.worldBox.max.x < b.worldBox.min.x || a.worldBox.min.x > b.worldBox.max.x) continue;
            if (a.worldBox.max.z < b.worldBox.min.z || a.worldBox.min.z > b.worldBox.max.z) continue;
            // Support sits below a's bottom.
            if (b.worldBox.max.y <= a.worldBox.min.y + 0.01f) {
                const double g = static_cast<double>(a.worldBox.min.y - b.worldBox.max.y);
                if (g < bestGap) {
                    bestGap = g;
                    supportName = b.name;
                }
            } else if (b.likelyGround && (b.worldBox.max.y - b.worldBox.min.y) > 0.5f && !b.node->bvh().empty()) {
                // Curved terrain or tall studio cyclorama backdrop: raycast down from bottom center.
                const bromath::Vec3 c{
                    0.5f * (a.worldBox.min.x + a.worldBox.max.x),
                    a.worldBox.min.y + 0.05f,
                    0.5f * (a.worldBox.min.z + a.worldBox.max.z),
                };
                const bromath::Mat4 invB = bromath::minverse(b.node->worldMatrix());
                const bromath::Vec3 lc = bromath::mtransformPoint(invB, c);
                const float orig[3] = {lc.x, lc.y, lc.z};
                const float dir[3] = {0.0f, -1.0f, 0.0f};
                const auto hit = b.node->bvh().raycast(b.node->mesh(), orig, dir, 50.0f);
                if (hit.hit) {
                    const bromath::Vec3 wp = bromath::mtransformPoint(b.node->worldMatrix(),
                        {hit.position[0], hit.position[1], hit.position[2]});
                    const double g = std::max(0.0, static_cast<double>(a.worldBox.min.y - wp.y));
                    if (g < bestGap) {
                        bestGap = g;
                        supportName = b.name;
                    }
                }
            } else if (!b.likelyGround && a.worldBox.min.y <= b.worldBox.max.y && a.worldBox.min.y >= b.worldBox.min.y - 0.02f) {
                // Resting inside or flush on a non-backdrop support mesh
                bestGap = 0.0;
                break;
            } else if (b.likelyGround && a.worldBox.min.y <= b.worldBox.max.y + 0.02f) {
                bestGap = 0.0;
                break;
            }
        }
        if (std::isfinite(bestGap) && bestGap > opts.floatMin && bestGap < opts.floatMax) {
            EyeIssue iss;
            iss.kind = "floating";
            iss.severity = "warning";
            iss.selector = a.name;
            iss.otherSelector = supportName;
            iss.nodeName = a.name;
            iss.otherNodeName = supportName;
            iss.hasSpatial = true;
            iss.gap = bestGap;
            iss.rect = res.nodes[i].bounds2d;
            char buf[180];
            std::snprintf(buf, sizeof(buf), "Node %s is floating %.3f units above %s",
                          a.name.c_str(), bestGap, supportName.c_str());
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    // 5. Interpenetration detection
    for (size_t i = 0; i < meshes.size(); ++i) {
        for (size_t j = i + 1; j < meshes.size(); ++j) {
            if (meshes[i].likelyGround || meshes[j].likelyGround) continue;
            const double depthOverlap = checkMeshInterpenetration(meshes[i], meshes[j], opts.penetrationEps);
            if (depthOverlap > opts.penetrationEps) {
                EyeIssue iss;
                iss.kind = "interpenetration";
                iss.severity = "error";
                iss.selector = meshes[i].name;
                iss.otherSelector = meshes[j].name;
                iss.nodeName = meshes[i].name;
                iss.otherNodeName = meshes[j].name;
                iss.hasSpatial = true;
                iss.gap = depthOverlap;
                iss.rect = res.nodes[i].bounds2d;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "Meshes %s and %s interpenetrate (overlap %.3f units)",
                              meshes[i].name.c_str(), meshes[j].name.c_str(), depthOverlap);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
        }
    }

    // 6. Optional sub-pixel camera jitter stability test (detects Z-fighting / aliasing shimmer)
    if (opts.checkStability) {
        scene::CameraNode* activeCam = graph.activeCamera();
        const bromath::Mat4 origV = graph.viewMatrix();
        const bromath::Mat4 origP = graph.projectionMatrix();
        const bromath::Vec3 origEye = graph.cameraEye();
        const float nearZ = graph.cameraNearZ();
        const float farZ = graph.cameraFarZ();
        const float fovY = graph.cameraFovY();
        const float p00 = origP.at(0, 0);
        const float p11 = origP.at(1, 1);
        const float aspect = (std::abs(p00) > 1e-6f && std::abs(p11) > 1e-6f) ? (p11 / p00) : 1.0f;

        const bromath::Vec3 right{origV.at(0, 0), origV.at(0, 1), origV.at(0, 2)};
        const bromath::Vec3 up{origV.at(1, 0), origV.at(1, 1), origV.at(1, 2)};
        const bromath::Vec3 fwd{-origV.at(2, 0), -origV.at(2, 1), -origV.at(2, 2)};

        const double refDist = res.medianDist > 0.1 ? res.medianDist : 5.0;
        const float worldPerPx = (2.0f * static_cast<float>(refDist) * std::tan(fovY * 0.5f)) / std::max(1, h);
        const float shift = static_cast<float>(opts.jitterPx) * worldPerPx;
        const bromath::Vec3 delta = right * shift + up * (shift * 0.7f);

        bromath::Vec3 savedCamPos{};
        if (activeCam) {
            savedCamPos = activeCam->position();
            activeCam->setPosition(savedCamPos + delta);
        } else if (graph.cameraIsPerspective()) {
            const bromath::Vec3 lookTarget = origEye + fwd * static_cast<float>(refDist);
            graph.setCamera(fovY, aspect, nearZ, farZ, origEye + delta, lookTarget + delta, up);
        }

        graph.renderForRead();
        std::vector<uint8_t> jRgba;
        std::vector<float> jDepth;
        int jw = 0, jh = 0;
        scene::vk::InspectCamera jCam{};
        if (graph.renderer().readInspectSnapshot(jRgba, jDepth, jw, jh, jCam) && jw == w && jh == h) {
            res.stability.tested = true;
            res.stability.jitterPx = opts.jitterPx;
            double deSum = 0.0, deMax = 0.0;
            int64_t changed = 0, depthFlips = 0, tot = 0;
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    const size_t idx = static_cast<size_t>(y) * w + x;
                    const double de = color::oklabDeltaE(
                        color::srgbToOklab(rgba[idx * 4], rgba[idx * 4 + 1], rgba[idx * 4 + 2]),
                        color::srgbToOklab(jRgba[idx * 4], jRgba[idx * 4 + 1], jRgba[idx * 4 + 2]));
                    deSum += de;
                    deMax = std::max(deMax, de);
                    if (de > 8.0) ++changed;

                    const float dz = std::abs(depth[idx] - jDepth[idx]);
                    if (depth[idx] > 1e-6f && jDepth[idx] > 1e-6f && dz > 0.01f) {
                        // Exclude natural grazing edges by checking local depth smoothness.
                        const float lapD = std::abs(depth[idx - 1] - depth[idx + 1]) + std::abs(depth[idx - w] - depth[idx + w]);
                        if (lapD < 0.002f) ++depthFlips;
                    }
                    ++tot;
                }
            }
            if (tot > 0) {
                res.stability.meanDeltaE = deSum / tot;
                res.stability.maxDeltaE = deMax;
                res.stability.changedRatio = static_cast<double>(changed) / tot;
                res.stability.depthFlipRatio = static_cast<double>(depthFlips) / tot;
                res.stability.shimmerScore = std::clamp(res.stability.changedRatio * 4.0 + res.stability.depthFlipRatio * 10.0, 0.0, 1.0);
            }
            if (res.stability.depthFlipRatio > 0.005) {
                EyeIssue iss;
                iss.kind = "z-fighting";
                iss.severity = "error";
                iss.value = res.stability.depthFlipRatio;
                iss.rect = {0, 0, static_cast<double>(w), static_cast<double>(h)};
                char buf[160];
                std::snprintf(buf, sizeof(buf), "Depth-buffer instability / Z-fighting under sub-pixel jitter (%.2f%%)",
                              res.stability.depthFlipRatio * 100.0);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
        }

        // Restore original camera cleanly.
        if (activeCam) {
            activeCam->setPosition(savedCamPos);
        } else if (graph.cameraIsPerspective()) {
            const bromath::Vec3 lookTarget = origEye + fwd * static_cast<float>(refDist);
            graph.setCamera(fovY, aspect, nearZ, farZ, origEye, lookTarget, up);
        }
        graph.renderForRead();
    }

    std::stable_sort(res.issues.begin(), res.issues.end(), [](const EyeIssue& a, const EyeIssue& b) {
        if (a.severity != b.severity) return a.severity == "error";
        return false;
    });
    for (const auto& iss : res.issues) {
        if (iss.severity == "error") ++res.errorCount;
        else ++res.warningCount;
    }
    if (opts.maxIssues > 0 && res.issues.size() > static_cast<size_t>(opts.maxIssues)) {
        res.issues.resize(static_cast<size_t>(opts.maxIssues));
    }
    res.ok = (res.errorCount == 0);
    res.score = std::clamp(100.0 - 15.0 * res.errorCount - 5.0 * res.warningCount, 0.0, 100.0);
    return res;
}

}  // namespace bro::eye
