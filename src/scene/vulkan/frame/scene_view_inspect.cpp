#include "scene/vulkan/frame/scene_view_inspect.h"

#include <algorithm>
#include <cmath>

namespace bro::scene::vk {

namespace {

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kSameFootprints = 2.5;     // a point within this many pixel footprints is the same
constexpr double kNormalMovedDeg = 15.0;
constexpr double kLumaMovedAt = 0.08;
constexpr int kBandCount = 4;

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double len3(const double a[3]) { return std::sqrt(dot3(a, a)); }
void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
// basis * v (column j of the row-major basis is axis j).
void toBodyAxes(const double basis[9], const double v[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = basis[r * 3] * v[0] + basis[r * 3 + 1] * v[1] + basis[r * 3 + 2] * v[2];
}
// basisᵀ * v.
void toPlaceAxes(const double basis[9], const double v[3], double out[3]) {
    for (int c = 0; c < 3; ++c) out[c] = basis[c] * v[0] + basis[3 + c] * v[1] + basis[6 + c] * v[2];
}
void loadVec(const bromath::Vec3& v, double out[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
bool hasNormal(const bromath::Vec3& n) { return n.x != 0.0f || n.y != 0.0f || n.z != 0.0f; }

uint8_t unit8(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); }

}  // namespace

void ViewInspector::build(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth,
                          const uint8_t* rgba, Frame& f) {
    f.w = width;
    f.h = height;
    toBodyAxes(cam.basis, cam.eye, f.eyeBody);
    for (int i = 0; i < 3; ++i) f.eyeBody[i] += cam.origin[i];
    std::copy(cam.basis, cam.basis + 9, f.basis);
    f.relViewProj = cam.relViewProj;
    // The view's rows are the camera's right, up and back in place axes.
    double rowR[3], rowU[3], rowB[3];
    for (int c = 0; c < 3; ++c) {
        rowR[c] = cam.relView.at(0, c);
        rowU[c] = cam.relView.at(1, c);
        rowB[c] = cam.relView.at(2, c);
    }
    toBodyAxes(f.basis, rowR, f.right);
    toBodyAxes(f.basis, rowU, f.up);
    double back[3];
    toBodyAxes(f.basis, rowB, back);
    for (int i = 0; i < 3; ++i) f.fwd[i] = -back[i];

    const size_t n = static_cast<size_t>(width) * height;
    f.off.assign(n, {});
    f.normal.assign(n, {});
    f.luma.resize(n);
    f.sky.assign(n, 0);
    const bromath::Mat4& ip = cam.invProj;
    for (uint32_t y = 0; y < height; ++y) {
        const double cy = 2.0 * (y + 0.5) / height - 1.0;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            const uint8_t* p = rgba + i * 4;
            f.luma[i] = static_cast<float>((0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0);
            const double cx = 2.0 * (x + 0.5) / width - 1.0;
            const float d = depth[i];
            const bool sky = !(d > 0.0f);
            // A sky pixel keeps its direction: its point on the near plane.
            const double cz = sky ? 1.0 : d;
            double v[4];
            for (int r = 0; r < 4; ++r)
                v[r] = ip.at(r, 0) * cx + ip.at(r, 1) * cy + ip.at(r, 2) * cz + ip.at(r, 3);
            if (!(std::abs(v[3]) > 0.0)) {
                f.sky[i] = 1;
                continue;
            }
            const double vx = v[0] / v[3], vy = v[1] / v[3], vz = v[2] / v[3];
            double rel[3];
            for (int c = 0; c < 3; ++c) rel[c] = rowR[c] * vx + rowU[c] * vy + rowB[c] * vz;
            double body[3];
            toBodyAxes(f.basis, rel, body);
            if (sky) {
                const double l = len3(body);
                if (l > 0.0)
                    for (double& c : body) c /= l;
                f.sky[i] = 1;
            }
            f.off[i] = {static_cast<float>(body[0]), static_cast<float>(body[1]), static_cast<float>(body[2])};
        }
    }

    // A surface's normal from its neighbours: on each axis the nearer one, so
    // a silhouette's far side does not bend it.
    auto step = [&](size_t i, size_t a, size_t b, bool hasA, bool hasB, double out[3]) {
        const bromath::Vec3& c = f.off[i];
        double da[3] = {0, 0, 0}, db[3] = {0, 0, 0};
        double la = -1.0, lb = -1.0;
        if (hasA && !f.sky[a]) {
            da[0] = c.x - f.off[a].x;
            da[1] = c.y - f.off[a].y;
            da[2] = c.z - f.off[a].z;
            la = len3(da);
        }
        if (hasB && !f.sky[b]) {
            db[0] = f.off[b].x - c.x;
            db[1] = f.off[b].y - c.y;
            db[2] = f.off[b].z - c.z;
            lb = len3(db);
        }
        if (la < 0.0 && lb < 0.0) return false;
        const double* pick = (lb < 0.0 || (la >= 0.0 && la <= lb)) ? da : db;
        std::copy(pick, pick + 3, out);
        return true;
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            if (f.sky[i]) continue;
            double dx[3], dy[3];
            if (!step(i, i - 1, i + 1, x > 0, x + 1 < width, dx) ||
                !step(i, i - width, i + width, y > 0, y + 1 < height, dy))
                continue;
            double nrm[3];
            cross3(dy, dx, nrm);
            const double l = len3(nrm);
            if (!(l > 0.0)) continue;
            double o[3];
            loadVec(f.off[i], o);
            const double s = dot3(nrm, o) > 0.0 ? -1.0 / l : 1.0 / l;   // facing the eye
            f.normal[i] = {static_cast<float>(nrm[0] * s), static_cast<float>(nrm[1] * s),
                           static_cast<float>(nrm[2] * s)};
        }
    }
}

// The least luma change against the previous render's pixels about
// (px, py): a pixel carried there lands within one of them, so an edge's
// half-pixel misplacement is no change, while a look that changed over an
// area still is.
double ViewInspector::lumaChange(float luma, int px, int py) const {
    const Frame& f = prev_;
    double best = 1.0;
    for (int y = std::max(py - 1, 0); y <= std::min(py + 1, static_cast<int>(f.h) - 1); ++y)
        for (int x = std::max(px - 1, 0); x <= std::min(px + 1, static_cast<int>(f.w) - 1); ++x)
            best = std::min(best, std::abs(static_cast<double>(luma) - f.luma[static_cast<size_t>(y) * f.w + x]));
    return best;
}

// The least turn (degrees) against the previous render's normals about
// (px, py), as lumaChange: detail finer than a pixel (grass) gives each
// pixel another blade's normal from frame to frame, which is no change of
// the surface.
double ViewInspector::normalChange(const bromath::Vec3& normal, int px, int py) const {
    const Frame& f = prev_;
    double n[3];
    loadVec(normal, n);
    double best = -2.0;
    for (int y = std::max(py - 1, 0); y <= std::min(py + 1, static_cast<int>(f.h) - 1); ++y)
        for (int x = std::max(px - 1, 0); x <= std::min(px + 1, static_cast<int>(f.w) - 1); ++x) {
            const bromath::Vec3& q = f.normal[static_cast<size_t>(y) * f.w + x];
            if (!hasNormal(q)) continue;
            double m[3];
            loadVec(q, m);
            best = std::max(best, dot3(n, m));
        }
    return best < -1.5 ? 0.0 : std::acos(std::clamp(best, -1.0, 1.0)) * kRadToDeg;
}

bool ViewInspector::projectPrev(const double bodyPoint[3], bool direction, int& px, int& py) const {
    const Frame& f = prev_;
    double rel[3];
    if (direction) {
        toPlaceAxes(f.basis, bodyPoint, rel);
    } else {
        const double d[3] = {bodyPoint[0] - f.eyeBody[0], bodyPoint[1] - f.eyeBody[1], bodyPoint[2] - f.eyeBody[2]};
        toPlaceAxes(f.basis, d, rel);
    }
    const double w1 = direction ? 0.0 : 1.0;
    double clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = f.relViewProj.at(r, 0) * rel[0] + f.relViewProj.at(r, 1) * rel[1] +
                  f.relViewProj.at(r, 2) * rel[2] + f.relViewProj.at(r, 3) * w1;
    if (!(clip[3] > 0.0)) return false;
    const double nx = clip[0] / clip[3], ny = clip[1] / clip[3];
    if (nx < -1.0 || nx >= 1.0 || ny < -1.0 || ny >= 1.0) return false;
    px = std::min(static_cast<int>((nx + 1.0) * 0.5 * f.w), static_cast<int>(f.w) - 1);
    py = std::min(static_cast<int>((ny + 1.0) * 0.5 * f.h), static_cast<int>(f.h) - 1);
    return true;
}

void ViewInspector::inspect(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth,
                            const uint8_t* rgba, Image image, std::vector<double>& stats,
                            std::vector<uint8_t>& outImage) {
    stats.assign(kStatCount, 0.0);
    stats[kWidth] = width;
    stats[kHeight] = height;
    if (width == 0 || height == 0) return;
    build(cam, width, height, depth, rgba, cur_);
    const Frame& c = cur_;
    const Frame& p = prev_;
    const size_t n = static_cast<size_t>(width) * height;
    const bool compare = prevW_ == width && prevH_ == height;
    stats[kCompared] = compare ? 1.0 : 0.0;
    // One pixel's angle at the centre of the view: invProj's (1,1) is the
    // projection's tan(fovy / 2).
    const double pixCur = 2.0 * std::abs(cam.invProj.at(1, 1)) / height;
    const double pixPrev = prevPix_;

    if (image != Image::None) outImage.assign(n * 4, 255);
    enum Class : uint8_t { kClsSame, kClsRose, kClsSank, kClsAppeared, kClsVanished, kClsNew, kClsSky };
    double count[7] = {};
    double normalSum = 0.0, normalMoved = 0.0, lumaSum = 0.0, lumaMoved = 0.0, skyLumaSum = 0.0;
    double band[kBandCount][3] = {};
    double bandPixels[kBandCount] = {};
    std::vector<float> dists;
    dists.reserve(n / 16 + 1);

    for (uint32_t y = 0; y < height; ++y) {
        const int b = std::min(static_cast<int>(y * kBandCount / height), kBandCount - 1);
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            bandPixels[b] += 1.0;
            double off[3];
            loadVec(c.off[i], off);
            const bool sky = c.sky[i] != 0;
            if (!sky && ((x | y) & 3) == 0) dists.push_back(static_cast<float>(len3(off)));
            Class cls = sky ? kClsSky : kClsNew;
            double angleDeg = 0.0, dl = 0.0;
            if (compare) {
                int qx = 0, qy = 0;
                if (sky) {
                    if (!projectPrev(off, true, qx, qy)) {
                        cls = kClsNew;
                    } else {
                        const size_t j = static_cast<size_t>(qy) * p.w + qx;
                        if (p.sky[j]) {
                            cls = kClsSky;
                            dl = lumaChange(c.luma[i], qx, qy);
                            skyLumaSum += dl;
                        } else {
                            cls = kClsVanished;
                        }
                    }
                } else {
                    double point[3];
                    for (int k = 0; k < 3; ++k) point[k] = c.eyeBody[k] + off[k];
                    if (!projectPrev(point, false, qx, qy)) {
                        cls = kClsNew;
                    } else {
                        const size_t j = static_cast<size_t>(qy) * p.w + qx;
                        if (p.sky[j]) {
                            cls = kClsAppeared;
                        } else {
                            double prevOff[3];
                            loadVec(p.off[j], prevOff);
                            double delta[3], fromPrevEye[3];
                            for (int k = 0; k < 3; ++k) {
                                fromPrevEye[k] = point[k] - p.eyeBody[k];
                                delta[k] = fromPrevEye[k] - prevOff[k];
                            }
                            const double distCur = len3(off);
                            const double distPrev = len3(prevOff);
                            const double footprint = std::max(distCur * pixCur, distPrev * pixPrev);
                            double facing = 1.0;
                            if (hasNormal(c.normal[i]) && distCur > 0.0) {
                                double nrm[3];
                                loadVec(c.normal[i], nrm);
                                facing = std::abs(dot3(nrm, off)) / distCur;
                            }
                            if (len3(delta) <= kSameFootprints * footprint / std::max(facing, 0.25)) {
                                cls = kClsSame;
                                if (hasNormal(c.normal[i])) angleDeg = normalChange(c.normal[i], qx, qy);
                                dl = lumaChange(c.luma[i], qx, qy);
                                normalSum += angleDeg;
                                lumaSum += dl;
                                if (angleDeg > kNormalMovedDeg) {
                                    normalMoved += 1.0;
                                    band[b][1] += 1.0;
                                }
                                if (dl > kLumaMovedAt) {
                                    lumaMoved += 1.0;
                                    band[b][2] += 1.0;
                                }
                            } else {
                                cls = len3(fromPrevEye) < distPrev ? kClsRose : kClsSank;
                            }
                        }
                    }
                }
            }
            count[cls] += 1.0;
            if (cls == kClsRose || cls == kClsSank || cls == kClsAppeared || cls == kClsVanished) band[b][0] += 1.0;

            if (image == Image::None) continue;
            uint8_t* o = outImage.data() + i * 4;
            if (image == Image::Normals) {
                if (sky || !hasNormal(c.normal[i])) {
                    o[0] = o[1] = o[2] = 0;
                    continue;
                }
                double nrm[3];
                loadVec(c.normal[i], nrm);
                // In the camera's axes: x right, y up, z toward the eye.
                o[0] = unit8(dot3(nrm, c.right) * 0.5 + 0.5);
                o[1] = unit8(dot3(nrm, c.up) * 0.5 + 0.5);
                o[2] = unit8(-dot3(nrm, c.fwd) * 0.5 + 0.5);
                continue;
            }
            // The change map over a dim copy of the frame.
            const double base = 0.3 * c.luma[i];
            double r = base, g = base, bl = base;
            switch (cls) {
            case kClsSame:
                r += angleDeg / 20.0;
                g += dl / 0.15;
                break;
            case kClsSky:
                g += dl / 0.15;
                break;
            case kClsRose: r = 0.0; g = 0.3; bl = 1.0; break;
            case kClsSank: r = 1.0; g = 0.0; bl = 1.0; break;
            case kClsAppeared: r = 0.0; g = 0.8; bl = 1.0; break;
            case kClsVanished: r = 1.0; g = 0.6; bl = 0.0; break;
            case kClsNew: break;
            }
            o[0] = unit8(r);
            o[1] = unit8(g);
            o[2] = unit8(bl);
        }
    }

    const double total = static_cast<double>(n);
    stats[kSame] = count[kClsSame] / total;
    stats[kRose] = count[kClsRose] / total;
    stats[kSank] = count[kClsSank] / total;
    stats[kAppeared] = count[kClsAppeared] / total;
    stats[kVanished] = count[kClsVanished] / total;
    stats[kNewView] = count[kClsNew] / total;
    stats[kSky] = count[kClsSky] / total;
    if (count[kClsSame] > 0.0) {
        stats[kNormalDeg] = normalSum / count[kClsSame];
        stats[kNormalMoved] = normalMoved / count[kClsSame];
        stats[kLuma] = lumaSum / count[kClsSame];
        stats[kLumaMoved] = lumaMoved / count[kClsSame];
    }
    if (compare && count[kClsSky] > 0.0) stats[kSkyLuma] = skyLumaSum / count[kClsSky];
    for (int b = 0; b < kBandCount; ++b)
        for (int k = 0; k < 3; ++k)
            stats[kBands + b * 3 + k] = bandPixels[b] > 0.0 ? band[b][k] / bandPixels[b] : 0.0;
    if (!dists.empty()) {
        std::nth_element(dists.begin(), dists.begin() + dists.size() / 2, dists.end());
        stats[kMedianDist] = dists[dists.size() / 2];
    }

    if (compare) {
        // The camera's turn, then its roll: the previous up carried by the
        // least rotation that takes the previous forward to this one, against
        // this up, about this forward.
        const double cosTurn = std::clamp(dot3(c.fwd, p.fwd), -1.0, 1.0);
        stats[kTurnDeg] = std::acos(cosTurn) * kRadToDeg;
        double axis[3];
        cross3(p.fwd, c.fwd, axis);
        const double sinTurn = len3(axis);
        double up[3] = {p.up[0], p.up[1], p.up[2]};
        if (sinTurn > 1e-12) {
            for (double& a : axis) a /= sinTurn;
            double kxv[3];
            cross3(axis, p.up, kxv);
            const double kv = dot3(axis, p.up);
            for (int k = 0; k < 3; ++k)
                up[k] = p.up[k] * cosTurn + kxv[k] * sinTurn + axis[k] * kv * (1.0 - cosTurn);
        }
        double turned[3];
        cross3(up, c.up, turned);
        stats[kRollDeg] = std::atan2(dot3(turned, c.fwd), std::clamp(dot3(up, c.up), -1.0, 1.0)) * kRadToDeg;
        double moved[3];
        for (int k = 0; k < 3; ++k) moved[k] = c.eyeBody[k] - p.eyeBody[k];
        stats[kTravel] = len3(moved);
    }

    std::swap(cur_, prev_);
    prevW_ = width;
    prevH_ = height;
    prevPix_ = pixCur;
}

}  // namespace bro::scene::vk
