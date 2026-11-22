#include "eye/eye_probe.h"
#include "eye/eye_color.h"
#include "eye/eye_draw_internal.h"

#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include <broimage/encode.h>
#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace bro::eye {
namespace {

struct PickFrame {
    bromath::Vec3 bx, by, bz, bt;
    bromath::Vec3 r0, r1, r2;
    bool ok = false;

    bromath::Vec3 pointToWorld(const bromath::Vec3& v) const {
        return bx * v.x + by * v.y + bz * v.z + bt;
    }
    bromath::Vec3 dirToLocal(const bromath::Vec3& v) const {
        return {bromath::vdot(v, r0), bromath::vdot(v, r1), bromath::vdot(v, r2)};
    }
    bromath::Vec3 pointToLocal(const bromath::Vec3& v) const {
        const bromath::Vec3 d = v - bt;
        return {bromath::vdot(d, r0), bromath::vdot(d, r1), bromath::vdot(d, r2)};
    }
    bromath::Vec3 normalToWorld(const bromath::Vec3& n) const {
        return r0 * n.x + r1 * n.y + r2 * n.z;
    }
};

PickFrame makePickFrame(const bromath::Mat4& m) {
    PickFrame f;
    const float* a = m.data;
    f.bx = {a[0], a[1], a[2]};
    f.by = {a[4], a[5], a[6]};
    f.bz = {a[8], a[9], a[10]};
    f.bt = {a[12], a[13], a[14]};
    const float det = bromath::vdot(f.bx, bromath::vcross(f.by, f.bz));
    if (std::abs(det) < 1e-12f) return f;
    const float inv = 1.0f / det;
    f.r0 = bromath::vcross(f.by, f.bz) * inv;
    f.r1 = bromath::vcross(f.bz, f.bx) * inv;
    f.r2 = bromath::vcross(f.bx, f.by) * inv;
    f.ok = true;
    return f;
}

bool slabHit(const bromath::AABB3& b, const bromath::Vec3& o, const bromath::Vec3& d, float& tNear) {
    float tmin = 0.0f, tmax = std::numeric_limits<float>::infinity();
    const float org[3] = {o.x, o.y, o.z};
    const float dir[3] = {d.x, d.y, d.z};
    const float bmin[3] = {b.min.x, b.min.y, b.min.z};
    const float bmax[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (org[i] < bmin[i] || org[i] > bmax[i]) return false;
        } else {
            const float inv = 1.0f / dir[i];
            float t1 = (bmin[i] - org[i]) * inv;
            float t2 = (bmax[i] - org[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tNear = tmin;
    return true;
}

std::string nodeDisplayName(const scene::SceneNode* n) {
    if (!n) return "";
    return !n->name().empty() ? n->name() : ("node#" + std::to_string(n->id()));
}

void raycastScene(scene::SceneGraph& graph, const bromath::Vec3& o, const bromath::Vec3& d,
                  EyeProbeSample& sample) {
    float closestDist = 1e30f;
    scene::SceneNode* closestNode = nullptr;
    bromath::Vec3 closestPoint{};
    bromath::Vec3 closestNormal{0, 1, 0};

    auto tryMesh = [&](const PickFrame& f, const bromesh::MeshData& md,
                       const bromath::AABB3& lb, auto&& bvhOf) -> bool {
        if (!f.ok) return false;
        const bromath::Vec3 lo = f.pointToLocal(o);
        const bromath::Vec3 ld = f.dirToLocal(d);
        const float ldLen = bromath::vlen(ld);
        if (ldLen < 1e-12f) return false;
        const bromath::Vec3 ldN = ld * (1.0f / ldLen);
        const float localMax = closestDist * ldLen;

        float tNear = 0.0f;
        if (!slabHit(lb, lo, ldN, tNear) || tNear > localMax) return false;

        const float op[3] = {lo.x, lo.y, lo.z};
        const float dp[3] = {ldN.x, ldN.y, ldN.z};
        const bromesh::RayHit hit = bvhOf().raycast(md, op, dp, localMax);
        if (!hit.hit) return false;

        const bromath::Vec3 worldHit = f.pointToWorld({hit.position[0], hit.position[1], hit.position[2]});
        const float worldDist = bromath::vlen(worldHit - o);
        if (worldDist >= closestDist) return false;

        closestDist = worldDist;
        closestPoint = worldHit;
        closestNormal = bromath::vnorm(f.normalToWorld({hit.normal[0], hit.normal[1], hit.normal[2]}));
        return true;
    };

    graph.root()->traverse([&](scene::SceneNode* node) {
        if (!node || !node->visible()) return;
        if (node->type() == scene::SceneNode::Type::Mesh) {
            auto* mn = static_cast<scene::MeshNode*>(node);
            const bromesh::MeshData& md = mn->mesh();
            if (md.positions.empty() || md.indices.empty()) return;
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return mn->bvh(); };
            if (tryMesh(makePickFrame(node->worldMatrix()), md, mn->localBounds(), bvhOf)) {
                closestNode = mn;
            }
        } else if (node->type() == scene::SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<scene::InstancedMeshNode*>(node);
            const bromesh::MeshData& md = im->mesh();
            if (md.positions.empty() || md.indices.empty() || im->instanceCount() == 0) return;
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return im->bvh(); };
            for (size_t i = 0; i < im->instanceCount(); ++i) {
                float rows[12];
                if (!im->instanceRows(i, rows)) continue;
                bromath::Mat4 instLocal = bromath::midentity();
                instLocal.at(0, 0) = rows[0]; instLocal.at(0, 1) = rows[1]; instLocal.at(0, 2) = rows[2]; instLocal.at(0, 3) = rows[3];
                instLocal.at(1, 0) = rows[4]; instLocal.at(1, 1) = rows[5]; instLocal.at(1, 2) = rows[6]; instLocal.at(1, 3) = rows[7];
                instLocal.at(2, 0) = rows[8]; instLocal.at(2, 1) = rows[9]; instLocal.at(2, 2) = rows[10]; instLocal.at(2, 3) = rows[11];
                const bromath::Mat4 instWorld = bromath::mmul(node->worldMatrix(), instLocal);
                if (tryMesh(makePickFrame(instWorld), md, im->localBounds(), bvhOf)) {
                    closestNode = im;
                }
            }
        }
    });

    if (closestNode) {
        sample.hit = true;
        sample.nodeName = nodeDisplayName(closestNode);
        sample.nodeId = closestNode->id();
        sample.point[0] = closestPoint.x;
        sample.point[1] = closestPoint.y;
        sample.point[2] = closestPoint.z;
        sample.normal[0] = closestNormal.x;
        sample.normal[1] = closestNormal.y;
        sample.normal[2] = closestNormal.z;
        sample.distance = closestDist;
    }
}

const EyeProbeSample* findSample(const std::vector<EyeProbeSample>& samples, const std::string& key) {
    for (const auto& s : samples) {
        if (!key.empty() && s.label == key) return &s;
    }
    char* end = nullptr;
    const long idx = std::strtol(key.c_str(), &end, 10);
    if (end != key.c_str() && *end == '\0' && idx >= 0 && static_cast<size_t>(idx) < samples.size()) {
        return &samples[static_cast<size_t>(idx)];
    }
    return nullptr;
}

}  // namespace

EyeProbeResult probeScene(scene::SceneGraph& graph, const EyeProbeOptions& opts) {
    EyeProbeResult res;
    if (opts.render) graph.renderForRead();

    int w = 0, h = 0;
    const std::vector<uint8_t> rgba = graph.readTonemapPixelsRGBA(w, h);
    if (rgba.empty() || w <= 0 || h <= 0) return res;

    res.width = w;
    res.height = h;

    std::vector<EyeProbePoint> pts = opts.points;
    if (pts.empty()) {
        const int cols = std::clamp(opts.gridCols, 1, 16);
        const int rows = std::clamp(opts.gridRows, 1, 16);
        pts.reserve(static_cast<size_t>(cols) * rows);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                EyeProbePoint p;
                p.x = (c + 0.5) / cols;
                p.y = (r + 0.5) / rows;
                p.normalized = true;
                char lbl[24];
                std::snprintf(lbl, sizeof(lbl), "g%d,%d", c, r);
                p.label = lbl;
                pts.push_back(std::move(p));
            }
        }
    }

    const int cw = graph.canvasWidth() > 0 ? graph.canvasWidth() : w;
    const int ch = graph.canvasHeight() > 0 ? graph.canvasHeight() : h;

    res.samples.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& pt = pts[i];
        const bool useNorm = pt.normalized && pt.x >= 0.0 && pt.x <= 1.0 && pt.y >= 0.0 && pt.y <= 1.0;
        const double nx = useNorm ? pt.x : (pt.x / std::max(1, w));
        const double ny = useNorm ? pt.y : (pt.y / std::max(1, h));

        EyeProbeSample s;
        s.index = static_cast<int>(i);
        s.label = !pt.label.empty() ? pt.label : ("p" + std::to_string(i));
        s.normX = std::clamp(nx, 0.0, 1.0);
        s.normY = std::clamp(ny, 0.0, 1.0);
        s.px = std::clamp(static_cast<int>(std::lround(s.normX * (w - 1))), 0, w - 1);
        s.py = std::clamp(static_cast<int>(std::lround(s.normY * (h - 1))), 0, h - 1);

        const size_t pxOff = (static_cast<size_t>(s.py) * w + s.px) * 4;
        s.r = rgba[pxOff + 0];
        s.g = rgba[pxOff + 1];
        s.b = rgba[pxOff + 2];
        s.hex = color::rgbToHex({s.r, s.g, s.b});
        const color::Oklab lab = color::srgbToOklab(s.r, s.g, s.b);
        s.L = lab.L;
        s.C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
        double hueDeg = std::atan2(lab.b, lab.a) * (180.0 / 3.14159265358979323846);
        if (hueDeg < 0.0) hueDeg += 360.0;
        s.h = (s.C > 1e-4) ? hueDeg : 0.0;

        bromath::Vec3 rayO{}, rayD{};
        if (graph.unprojectLocal(static_cast<float>(s.normX * cw), static_cast<float>(s.normY * ch), rayO, rayD)) {
            raycastScene(graph, rayO, bromath::vnorm(rayD), s);
        }
        res.samples.push_back(std::move(s));
    }

    auto addDistance = [&](const EyeProbeSample* sa, const EyeProbeSample* sb) {
        if (!sa || !sb || !sa->hit || !sb->hit) return;
        EyeProbeDistance d;
        d.a = sa->label;
        d.b = sb->label;
        d.dx = sb->point[0] - sa->point[0];
        d.dy = sb->point[1] - sa->point[1];
        d.dz = sb->point[2] - sa->point[2];
        d.distance = std::sqrt(d.dx * d.dx + d.dy * d.dy + d.dz * d.dz);
        res.distances.push_back(std::move(d));
    };

    if (!opts.pairs.empty()) {
        for (const auto& [ka, kb] : opts.pairs) {
            addDistance(findSample(res.samples, ka), findSample(res.samples, kb));
        }
    } else if (!opts.points.empty() && res.samples.size() >= 2 && res.samples.size() <= 16) {
        for (size_t i = 0; i + 1 < res.samples.size(); ++i) {
            addDistance(&res.samples[i], &res.samples[i + 1]);
        }
    }

    if (!opts.path.empty()) {
        EyeImage canvasImg = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, opts.maxPanelDim);
        for (const auto& d : res.distances) {
            const EyeProbeSample* sa = findSample(res.samples, d.a);
            const EyeProbeSample* sb = findSample(res.samples, d.b);
            if (!sa || !sb) continue;
            const int x0 = static_cast<int>(std::lround(sa->normX * canvasImg.width));
            const int y0 = static_cast<int>(std::lround(sa->normY * canvasImg.height));
            const int x1 = static_cast<int>(std::lround(sb->normX * canvasImg.width));
            const int y1 = static_cast<int>(std::lround(sb->normY * canvasImg.height));
            draw::drawLine(canvasImg, x0, y0, x1, y1, 2, 100, 235, 255, 220);
            char dbuf[32];
            std::snprintf(dbuf, sizeof(dbuf), "%.2fu", d.distance);
            draw::drawText(canvasImg, std::clamp((x0 + x1) / 2 + 4, 2, std::max(2, canvasImg.width - 64)),
                           std::clamp((y0 + y1) / 2 - 10, 2, std::max(2, canvasImg.height - 14)),
                           dbuf, 1, 100, 235, 255, true);
        }
        for (const auto& s : res.samples) {
            const int sx = std::clamp(static_cast<int>(std::lround(s.normX * canvasImg.width)), 4, canvasImg.width - 5);
            const int sy = std::clamp(static_cast<int>(std::lround(s.normY * canvasImg.height)), 4, canvasImg.height - 5);
            const uint8_t cr = s.hit ? 255 : 220;
            const uint8_t cg = s.hit ? 215 : 80;
            const uint8_t cb = s.hit ? 65 : 80;
            draw::drawCircle(canvasImg, sx, sy, 6, 2, cr, cg, cb, 240);
            draw::drawLine(canvasImg, sx - 9, sy, sx + 9, sy, 1, cr, cg, cb, 220);
            draw::drawLine(canvasImg, sx, sy - 9, sx, sy + 9, 1, cr, cg, cb, 220);
            char buf[96];
            if (s.hit) {
                std::snprintf(buf, sizeof(buf), "%s:(%.2f,%.2f,%.2f)", s.label.c_str(), s.point[0], s.point[1], s.point[2]);
            } else {
                std::snprintf(buf, sizeof(buf), "%s:miss", s.label.c_str());
            }
            draw::drawText(canvasImg, std::clamp(sx + 8, 2, std::max(2, canvasImg.width - 170)),
                           std::clamp(sy - 12, 2, std::max(2, canvasImg.height - 14)),
                           buf, 1, cr, cg, cb, true);
        }
        res.visual = std::move(canvasImg);
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
