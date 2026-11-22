#include "eye/eye_trace.h"
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
#include <unordered_map>
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
    if (needle.empty()) return true;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

struct PaletteColor { uint8_t r, g, b; };
constexpr PaletteColor kPalette[] = {
    {255, 118, 72}, {86, 224, 245}, {250, 210, 75}, {140, 245, 130},
    {235, 130, 245}, {255, 175, 110}, {110, 185, 255}, {245, 245, 120},
};

}  // namespace

EyeTraceResult traceSceneMotion(scene::SceneGraph& graph, const EyeTraceOptions& opts,
                                const std::function<void(int, int)>& stepFn) {
    EyeTraceResult res;
    const int steps = std::clamp(opts.steps, 2, 120);
    res.steps = steps;

    struct TrackSample {
        scene::MeshNode* node = nullptr;
        std::string name;
        bromath::AABB3 initialBox{};
        std::vector<bromath::Vec3> pts;
        std::vector<bromath::Vec3> fwds;
        std::vector<bromath::Vec3> snapPts;
    };
    std::unordered_map<uint32_t, TrackSample> byId;
    std::vector<uint32_t> order;

    EyeImage baseImg;
    int cw = graph.canvasWidth(), ch = graph.canvasHeight();

    for (int k = 0; k < steps; ++k) {
        if (stepFn) stepFn(k, steps);
        if (k == 0 && !opts.path.empty()) {
            graph.renderForRead();
            int w = 0, h = 0;
            const auto rgba = graph.readTonemapPixelsRGBA(w, h);
            if (!rgba.empty() && w > 0 && h > 0) {
                cw = w;
                ch = h;
                const int targetW = std::clamp(opts.sheetWidth, 128, 1280);
                const int targetH = std::clamp(opts.sheetHeight, 128, 1280);
                baseImg = draw::fitBox(EyeImageView{rgba.data(), w, h, w * 4}, targetW, targetH);
            }
        }
        graph.root()->traverse([&](scene::SceneNode* node) {
            if (!node || !node->visible() || node->type() != scene::SceneNode::Type::Mesh) return;
            auto* mn = static_cast<scene::MeshNode*>(node);
            if (mn->mesh().empty()) return;
            const bromath::Mat4 wm = mn->worldMatrix();
            const bromath::AABB3 wb = worldAabb(mn->localBounds(), wm);
            const bromath::Vec3 c = bromath::acenter(wb);
            const bromath::Vec3 rawFwd{wm.data[0], wm.data[1], wm.data[2]};
            const float fwdLen = bromath::vlen(rawFwd);
            const bromath::Vec3 fwd = fwdLen > 1e-5f ? (rawFwd * (1.0f / fwdLen)) : bromath::Vec3{1.0f, 0.0f, 0.0f};
            auto it = byId.find(mn->id());
            if (it == byId.end()) {
                TrackSample ts;
                ts.node = mn;
                ts.name = mn->name().empty() ? ("mesh#" + std::to_string(mn->id())) : mn->name();
                ts.initialBox = wb;
                ts.pts.push_back(c);
                ts.fwds.push_back(fwd);
                order.push_back(mn->id());
                byId.emplace(mn->id(), std::move(ts));
            } else {
                it->second.pts.push_back(c);
                it->second.fwds.push_back(fwd);
            }
        });
    }

    // Separate static reference meshes from dynamic moving meshes
    struct StaticRef {
        std::string name;
        bromath::AABB3 box{};
        bromath::Vec3 center{};
    };
    std::vector<StaticRef> statics;
    std::vector<TrackSample*> dynamicSamples;

    for (uint32_t id : order) {
        auto& ts = byId[id];
        double total = 0.0;
        for (size_t i = 1; i < ts.pts.size(); ++i) {
            total += bromath::vlen(ts.pts[i] - ts.pts[i - 1]);
        }
        if (total < opts.minTravel) {
            statics.push_back({ts.name, ts.initialBox, bromath::acenter(ts.initialBox)});
        } else if (containsCI(ts.name, opts.filter)) {
            dynamicSamples.push_back(&ts);
        }
    }

    int totalSnaps = 0;
    for (TrackSample* ts : dynamicSamples) {
        if (ts->pts.empty()) continue;
        EyeTraceTrack tr;
        tr.name = ts->name;
        const bromath::Vec3& p0 = ts->pts.front();
        const bromath::Vec3& p1 = ts->pts.back();
        tr.startPos[0] = p0.x; tr.startPos[1] = p0.y; tr.startPos[2] = p0.z;
        tr.endPos[0] = p1.x; tr.endPos[1] = p1.y; tr.endPos[2] = p1.z;

        double minY = p0.y, maxY = p0.y;
        std::vector<double> stepLens;
        stepLens.reserve(ts->pts.size());
        double sum = 0.0, maxStep = 0.0;
        for (size_t i = 0; i < ts->pts.size(); ++i) {
            minY = std::min(minY, static_cast<double>(ts->pts[i].y));
            maxY = std::max(maxY, static_cast<double>(ts->pts[i].y));
            if (i > 0) {
                const double d = bromath::vlen(ts->pts[i] - ts->pts[i - 1]);
                stepLens.push_back(d);
                sum += d;
                maxStep = std::max(maxStep, d);
            }
        }
        tr.minY = minY;
        tr.maxY = maxY;
        tr.pathLength = sum;
        tr.maxStepJump = maxStep;
        const double meanStep = stepLens.empty() ? 0.0 : sum / static_cast<double>(stepLens.size());
        const double jumpThresh = std::max(0.35, meanStep * 4.0);
        std::vector<double> normalSteps;
        int teleports = 0;
        for (double sl : stepLens) {
            if (sl > jumpThresh) ++teleports;
            else normalSteps.push_back(sl);
        }
        double medStep = meanStep;
        if (!normalSteps.empty()) {
            std::sort(normalSteps.begin(), normalSteps.end());
            medStep = normalSteps[normalSteps.size() / 2];
        }
        tr.teleportCount = teleports;
        tr.closedLoop = bromath::vlen(p1 - p0) < 0.12;
        tr.touchesWater = (minY <= opts.waterY + 0.03) && (maxY >= opts.waterY - 0.03);

        // C0/C1 continuity + rotation snap across non-teleport consecutive pairs
        double maxVelJump = 0.0, maxRotDeg = 0.0;
        int snaps = 0;
        constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
        const double velThresh = std::max(0.055, medStep * 2.6);
        for (size_t i = 1; i + 1 < ts->pts.size(); ++i) {
            const double dPrev = stepLens[i - 1];
            const double dNext = stepLens[i];
            if (dPrev > jumpThresh || dNext > jumpThresh) continue;
            const bromath::Vec3 vPrev = ts->pts[i] - ts->pts[i - 1];
            const bromath::Vec3 vNext = ts->pts[i + 1] - ts->pts[i];
            const double velJump = bromath::vlen(vNext - vPrev);
            maxVelJump = std::max(maxVelJump, velJump);
            const double cosA = std::clamp(static_cast<double>(bromath::vdot(ts->fwds[i], ts->fwds[i - 1])), -1.0, 1.0);
            const double rotDeg = std::acos(cosA) * kRadToDeg;
            maxRotDeg = std::max(maxRotDeg, rotDeg);
            if (velJump > velThresh || rotDeg > 35.0) {
                ++snaps;
                ts->snapPts.push_back(ts->pts[i]);
            }
        }
        tr.maxVelJump = maxVelJump;
        tr.maxRotSnapDeg = maxRotDeg;
        tr.snapCount = snaps;
        totalSnaps += snaps;

        double bestDist = std::numeric_limits<double>::infinity();
        std::string bestStatic;
        for (const auto& sr : statics) {
            const float dx = std::max(0.0f, std::max(sr.box.min.x - p0.x, p0.x - sr.box.max.x));
            const float dy = std::max(0.0f, std::max(sr.box.min.y - p0.y, p0.y - sr.box.max.y));
            const float dz = std::max(0.0f, std::max(sr.box.min.z - p0.z, p0.z - sr.box.max.z));
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < bestDist) {
                bestDist = dist;
                bestStatic = sr.name;
            }
        }
        tr.nearestStartStatic = bestStatic;
        tr.nearestStartDist = std::isfinite(bestDist) ? bestDist : 0.0;
        res.tracks.push_back(std::move(tr));
    }

    res.trackCount = static_cast<int>(res.tracks.size());

    if (!opts.path.empty() && !baseImg.rgba.empty()) {
        // Re-set step 0 camera pose to ensure projection matches baseImg
        if (stepFn) stepFn(0, steps);
        const int targetW = std::clamp(opts.sheetWidth, 128, 1280);
        const int targetH = std::clamp(opts.sheetHeight, 128, 1280);
        EyeImage canvas{targetW, targetH, std::vector<uint8_t>(static_cast<size_t>(targetW) * targetH * 4, 20)};
        for (size_t i = 3; i < canvas.rgba.size(); i += 4) canvas.rgba[i] = 255;

        // Dim base frame slightly so vector trajectories stand out clearly
        const int ox = std::max(0, (targetW - baseImg.width) / 2);
        const int oy = std::max(0, (targetH - baseImg.height) / 2);
        for (int y = 0; y < baseImg.height; ++y) {
            for (int x = 0; x < baseImg.width; ++x) {
                const size_t si = (static_cast<size_t>(y) * baseImg.width + x) * 4;
                const size_t di = (static_cast<size_t>(oy + y) * targetW + (ox + x)) * 4;
                canvas.rgba[di + 0] = static_cast<uint8_t>(baseImg.rgba[si + 0] * 0.65);
                canvas.rgba[di + 1] = static_cast<uint8_t>(baseImg.rgba[si + 1] * 0.65);
                canvas.rgba[di + 2] = static_cast<uint8_t>(baseImg.rgba[si + 2] * 0.65);
                canvas.rgba[di + 3] = 255;
            }
        }

        const float cwf = static_cast<float>(cw > 0 ? cw : baseImg.width);
        const float chf = static_cast<float>(ch > 0 ? ch : baseImg.height);
        auto project = [&](const bromath::Vec3& p, int& px, int& py) -> bool {
            float lx = 0, ly = 0, d = 0;
            if (!graph.projectLocal(p, lx, ly, d) || d <= 0.0f) return false;
            px = ox + static_cast<int>(std::lround((lx / cwf) * baseImg.width));
            py = oy + static_cast<int>(std::lround((ly / chf) * baseImg.height));
            return true;
        };

        for (size_t ti = 0; ti < dynamicSamples.size(); ++ti) {
            const TrackSample* ts = dynamicSamples[ti];
            const auto& col = kPalette[ti % (sizeof(kPalette) / sizeof(kPalette[0]))];
            int prevX = 0, prevY = 0;
            bool hasPrev = false;
            for (size_t pi = 0; pi < ts->pts.size(); ++pi) {
                int x = 0, y = 0;
                if (!project(ts->pts[pi], x, y)) {
                    hasPrev = false;
                    continue;
                }
                if (hasPrev) {
                    const double segJump = bromath::vlen(ts->pts[pi] - ts->pts[pi - 1]);
                    if (segJump <= std::max(0.35, res.tracks[ti].pathLength / std::max(1, steps) * 4.0)) {
                        draw::drawLine(canvas, prevX, prevY, x, y, 2, col.r, col.g, col.b, 230);
                    }
                }
                if (pi == 0) draw::drawCircle(canvas, x, y, 4, 2, 100, 255, 140, 255);
                if (pi + 1 == ts->pts.size()) draw::drawRectBox(canvas, x - 3, y - 3, 7, 7, 2, 255, 90, 90);
                prevX = x; prevY = y;
                hasPrev = true;
            }
            for (const auto& sp : ts->snapPts) {
                int sx = 0, sy = 0;
                if (project(sp, sx, sy)) {
                    draw::drawCircle(canvas, sx, sy, 5, 2, 255, 60, 220, 255);
                }
            }
        }

        char hdr[112];
        std::snprintf(hdr, sizeof(hdr), "TRACE | %d tracks | %d steps | snaps=%d | start=green end=red",
                      res.trackCount, steps, totalSnaps);
        draw::drawText(canvas, 8, 8, hdr, 1, 230, 240, 250, true);

        res.width = canvas.width;
        res.height = canvas.height;
        res.visual = {canvas.width, canvas.height, std::move(canvas.rgba)};
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
