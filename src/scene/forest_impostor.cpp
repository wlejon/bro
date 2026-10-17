#include "scene/forest_impostor.h"

#include "scene/forest_node.h"

#include <bromath/bromath.h>
#include <bromesh/procedural/leaf_clumps.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace bro::scene {

using bromath::Vec3;

namespace {

constexpr int kSuper = 2;   // samples a texel a side

float fract(float x) { return x - std::floor(x); }
float smooth01(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
uint8_t toSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return toByte(s);
}

// One specimen's surfaces in specimen space, ready for every view.
struct Prepared {
    Vec3 center{};
    float radius = 1.0f;
    float across = 1.0f;   // farthest reach from the upright axis through the centre
    float upDown = 1.0f;   // and above or below the centre
    // Leaves: kV vertices each (positions, normals), and a front and an
    // underside albedo.
    std::vector<Vec3> pos, nrm;
    std::vector<Vec3> front, back;
    // Bark: (from, rFrom), (to, rTo).
    std::vector<float> bark;
};

// The leaf colour's per-leaf multiplier: scene_vegetation.glsl's
// vegetationTint for a leaf, less its per-tree part (vegTreeTint).
Vec3 leafTint(const float* veg, const Vec3& anchor) {
    const uint32_t a = static_cast<uint32_t>(std::max(veg[13], 0.0f) + 0.5f);
    const float phase = static_cast<float>(a >> 18) / 64.0f;
    const float limb = fract(phase * 7.31f + 0.37f);
    const float each = fract(std::sin(anchor.x * 12.9898f + anchor.y * 78.233f + anchor.z * 37.719f) * 43758.5453f);
    float k = (0.8f + 0.4f * limb) * (0.9f + 0.2f * each);
    Vec3 c{k, k, k};
    if (veg[7] > 0.0f) {
        const Vec3 crown{veg[4], veg[5], veg[6]};
        const float depth = bromath::vlen(anchor - crown) / std::max(veg[7], 0.33f);
        const float t = smooth01(0.25f, 1.0f, depth);
        c.x *= 0.5f + 0.5f * t;
        c.y *= 0.62f + 0.38f * t;
        c.z *= 0.68f + 0.32f * t;
        c = c * (1.0f + 0.15f * smooth01(0.85f, 1.15f, depth));
    }
    return c;
}

Prepared prepare(const ForestNode& node, const ForestNode::Specimen& s) {
    Prepared p;
    const ForestNode::SpecimenArrays& a = node.arrays();
    const bromesh::MeshData& leaf = node.mesh();
    const size_t kV = leaf.vertexCount();
    const float* lc = node.color();
    const Vec3 leafColor{lc[0], lc[1], lc[2]};

    bromesh::LeafClumpOptions opts;
    opts.seed = a.leaf.seed;
    opts.upBias = a.leaf.upBias;
    opts.tiltJitter = a.leaf.tiltJitter;
    opts.shell = a.leaf.shell;
    opts.baseScale = a.leaf.baseScale;
    opts.scaleJitter = a.leaf.scaleJitter;

    float rec[16];
    p.pos.reserve(static_cast<size_t>(s.leafCount) * kV);
    p.nrm.reserve(static_cast<size_t>(s.leafCount) * kV);
    for (uint32_t i = s.leafStart; i < s.leafStart + s.leafCount; ++i) {
        const uint32_t clump = a.leaves[2 * i], key = a.leaves[2 * i + 1];
        const float* c = &a.clumps[clump * 8];
        bromesh::LeafClump lcmp;
        lcmp.center = {c[0], c[1], c[2]};
        lcmp.radius = c[3];
        lcmp.dir = {c[4], c[5], c[6]};
        if (!bromesh::leafInClump(lcmp, clump, key, opts, rec)) continue;
        const Vec3 P{rec[3], rec[7], rec[11]};
        for (size_t v = 0; v < kV; ++v) {
            const float* q = &leaf.positions[v * 3];
            p.pos.push_back({rec[0] * q[0] + rec[1] * q[1] + rec[2] * q[2] + P.x,
                             rec[4] * q[0] + rec[5] * q[1] + rec[6] * q[2] + P.y,
                             rec[8] * q[0] + rec[9] * q[1] + rec[10] * q[2] + P.z});
            Vec3 n{0.0f, 1.0f, 0.0f};
            if (leaf.normals.size() >= (v + 1) * 3) n = {leaf.normals[v * 3], leaf.normals[v * 3 + 1], leaf.normals[v * 3 + 2]};
            n = {rec[0] * n.x + rec[1] * n.y + rec[2] * n.z, rec[4] * n.x + rec[5] * n.y + rec[6] * n.z,
                 rec[8] * n.x + rec[9] * n.y + rec[10] * n.z};
            p.nrm.push_back(bromath::vnormOr(n, Vec3{0.0f, 1.0f, 0.0f}));
        }
        // As mesh.frag shades it: the tint, and an underside paler and
        // yellower than the top.
        const Vec3 tint = leafTint(&a.clumpVegetation[clump * 16], P);
        const Vec3 f{leafColor.x * tint.x, leafColor.y * tint.y, leafColor.z * tint.z};
        const Vec3 under{f.x * 1.25f + 0.015f, f.y * 1.3f + 0.02f, f.z * 1.05f};
        p.front.push_back(f);
        p.back.push_back(f + (under - f) * 0.6f);
    }

    for (uint32_t g = s.segStart; g < s.segStart + s.segCount; ++g) {
        const float* r = &a.segments[g * 8];
        p.bark.insert(p.bark.end(), r, r + 8);
    }

    // The card: round the middle of everything drawn, out to its farthest point.
    bromath::AABB3 box = bromath::aempty3();
    for (const Vec3& v : p.pos) box = bromath::amerge(box, bromath::AABB3{v, v});
    for (size_t k = 0; k + 7 < p.bark.size(); k += 8) {
        for (int e = 0; e < 2; ++e) {
            const Vec3 v{p.bark[k + e * 4], p.bark[k + e * 4 + 1], p.bark[k + e * 4 + 2]};
            const float r = p.bark[k + e * 4 + 3];
            box = bromath::amerge(box, bromath::AABB3{v - Vec3{r, r, r}, v + Vec3{r, r, r}});
        }
    }
    if (bromath::aisEmpty(box)) box = s.bounds;
    p.center = (box.min + box.max) * 0.5f;
    float far2 = 0.0f, across = 0.0f, upDown = 0.0f;
    const auto reach = [&](const Vec3& v, float r) {
        const Vec3 q = v - p.center;
        const float d = bromath::vlen(q) + r;
        far2 = std::max(far2, d * d);
        across = std::max(across, std::hypot(q.x, q.z) + r);
        upDown = std::max(upDown, std::fabs(q.y) + r);
    };
    for (const Vec3& v : p.pos) reach(v, 0.0f);
    for (size_t k = 0; k + 7 < p.bark.size(); k += 8) {
        for (int e = 0; e < 2; ++e)
            reach(Vec3{p.bark[k + e * 4], p.bark[k + e * 4 + 1], p.bark[k + e * 4 + 2]}, p.bark[k + e * 4 + 3]);
    }
    p.radius = std::max(std::sqrt(far2) * 1.03f, 0.05f);
    p.across = std::min(std::max(across * 1.03f, 0.05f), p.radius);
    p.upDown = std::min(std::max(upDown * 1.03f, 0.05f), p.radius);
    return p;
}

// What a cell holds before it is packed: kSuper x kSuper samples a texel.
struct Raster {
    int S = 0;
    std::vector<float> z;
    std::vector<Vec3> n, c;
    std::vector<uint8_t> leafy;
    void reset(int side) {
        S = side;
        const size_t k = static_cast<size_t>(side) * side;
        z.assign(k, -1e30f);
        n.assign(k, Vec3{});
        c.assign(k, Vec3{});
        leafy.assign(k, 0);
    }
};

struct View {
    Vec3 f, right, up, center;
    float half = 1.0f;   // the card's half size
    int S = 0;
    // Specimen point to (x, y) samples and depth toward the view.
    void project(const Vec3& p, float& x, float& y, float& d) const {
        const Vec3 q = p - center;
        x = (bromath::vdot(q, right) / half * 0.5f + 0.5f) * static_cast<float>(S);
        y = (0.5f - bromath::vdot(q, up) / half * 0.5f) * static_cast<float>(S);
        d = bromath::vdot(q, f);
    }
};

void drawLeaves(const Prepared& p, const bromesh::MeshData& leaf, const View& v, Raster& r) {
    const size_t kV = leaf.vertexCount();
    if (kV == 0) return;
    std::vector<uint32_t> idx = leaf.indices;
    if (idx.empty()) {
        idx.resize(kV);
        for (uint32_t i = 0; i < kV; ++i) idx[i] = i;
    }
    const size_t leaves = p.front.size();
    std::vector<float> sx(kV), sy(kV), sz(kV);
    for (size_t l = 0; l < leaves; ++l) {
        const Vec3* P = &p.pos[l * kV];
        const Vec3* N = &p.nrm[l * kV];
        for (size_t k = 0; k < kV; ++k) v.project(P[k], sx[k], sy[k], sz[k]);
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
            const float x0 = sx[i0], y0 = sy[i0], x1 = sx[i1], y1 = sy[i1], x2 = sx[i2], y2 = sy[i2];
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(area) < 1e-9f) continue;
            const int minX = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}) - 0.5f)));
            const int maxX = std::min(r.S - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}) - 0.5f)));
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}) - 0.5f)));
            const int maxY = std::min(r.S - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}) - 0.5f)));
            const float inv = 1.0f / area;
            for (int y = minY; y <= maxY; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                for (int x = minX; x <= maxX; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
                    const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float d = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2];
                    const size_t o = static_cast<size_t>(y) * r.S + x;
                    if (d <= r.z[o]) continue;
                    Vec3 n = N[i0] * w0 + N[i1] * w1 + N[i2] * w2;
                    // Two-sided: the side the view sees, its underside paler.
                    const bool under = bromath::vdot(n, v.f) < 0.0f;
                    if (under) n = n * -1.0f;
                    r.z[o] = d;
                    r.n[o] = bromath::vnormOr(n, v.f);
                    r.c[o] = under ? p.back[l] : p.front[l];
                    r.leafy[o] = 1;
                }
            }
        }
    }
}

void drawBark(const Prepared& p, const View& v, const Vec3& barkColor, Raster& r) {
    const float perSample = 2.0f * v.half / static_cast<float>(r.S);   // specimen units a sample
    for (size_t k = 0; k + 7 < p.bark.size(); k += 8) {
        const float* s = &p.bark[k];
        if (std::max(s[3], s[7]) < 0.25f * perSample) continue;
        float ax, ay, az, bx, by, bz;
        v.project({s[0], s[1], s[2]}, ax, ay, az);
        v.project({s[4], s[5], s[6]}, bx, by, bz);
        const float ra = s[3] / perSample, rb = s[7] / perSample;
        const float rm = std::max(ra, rb);
        const int minX = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - rm)));
        const int maxX = std::min(r.S - 1, static_cast<int>(std::ceil(std::max(ax, bx) + rm)));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - rm)));
        const int maxY = std::min(r.S - 1, static_cast<int>(std::ceil(std::max(ay, by) + rm)));
        const float dx = bx - ax, dy = by - ay, dd = dx * dx + dy * dy;
        for (int y = minY; y <= maxY; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = minX; x <= maxX; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float t = dd > 1e-9f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / dd, 0.0f, 1.0f) : 0.0f;
                const float ox = px - (ax + dx * t), oy = py - (ay + dy * t);
                const float rr = ra + (rb - ra) * t;
                const float dist2 = ox * ox + oy * oy;
                if (rr <= 0.0f || dist2 >= rr * rr) continue;
                const float h = std::sqrt(rr * rr - dist2);
                const float d = az + (bz - az) * t + h * perSample;
                const size_t o = static_cast<size_t>(y) * r.S + x;
                if (d <= r.z[o]) continue;
                r.z[o] = d;
                r.n[o] = bromath::vnormOr(v.right * (ox / rr) - v.up * (oy / rr) + v.f * (h / rr), v.f);
                r.c[o] = barkColor;
                r.leafy[o] = 0;
            }
        }
    }
}

// Resolve the samples into cell (cx, cy) of the atlas.
void pack(const Raster& r, const View& v, int R, float leafLength, ForestImpostorAtlas& out, int ox, int oy) {
    // The crown's own shade, which no single depth a texel keeps can give
    // back: a leaf seen through a gap, behind the leaves round it, is in
    // their shade. The front round each sample is the nearest surface
    // within a couple of leaves (a separable max of the depth); a leaf
    // behind it darkens with how far.
    const int S = r.S;
    std::vector<float> front(r.z);
    {
        const float pxPerUnit = static_cast<float>(S) / (2.0f * v.half);
        const int rad = std::clamp(static_cast<int>(std::lround(2.0f * leafLength * pxPerUnit)), 1, 12);
        std::vector<float> tmp(front.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < S; ++y) {
                for (int x = 0; x < S; ++x) {
                    float m = -1e30f;
                    for (int k = -rad; k <= rad; ++k) {
                        const int xx = pass == 0 ? x + k : x, yy = pass == 0 ? y : y + k;
                        if (xx < 0 || yy < 0 || xx >= S || yy >= S) continue;
                        m = std::max(m, front[static_cast<size_t>(yy) * S + xx]);
                    }
                    tmp[static_cast<size_t>(y) * S + x] = m;
                }
            }
            front.swap(tmp);
        }
    }
    const float shadeDepth = std::max(0.08f * v.half, 1e-3f);
    std::vector<float> cov(static_cast<size_t>(R) * R, 0.0f);
    std::vector<Vec3> col(cov.size()), nrm(cov.size());
    std::vector<float> dep(cov.size(), 0.0f), leaf(cov.size(), 0.0f);
    for (int y = 0; y < R; ++y) {
        for (int x = 0; x < R; ++x) {
            int n = 0, nl = 0;
            Vec3 c{}, nn{};
            float d = 0.0f;
            for (int j = 0; j < kSuper; ++j) {
                for (int i = 0; i < kSuper; ++i) {
                    const size_t o = static_cast<size_t>(y * kSuper + j) * r.S + (x * kSuper + i);
                    if (r.z[o] < -1e29f) continue;
                    ++n;
                    const float shade =
                        r.leafy[o] ? 0.55f + 0.45f * std::exp(-(front[o] - r.z[o]) / shadeDepth) : 1.0f;
                    c = c + r.c[o] * shade;
                    nn = nn + r.n[o];
                    d += r.z[o];
                    nl += r.leafy[o];
                }
            }
            const size_t t = static_cast<size_t>(y) * R + x;
            if (n == 0) continue;
            const float inv = 1.0f / static_cast<float>(n);
            cov[t] = static_cast<float>(n) / (kSuper * kSuper);
            col[t] = c * inv;
            nrm[t] = bromath::vnormOr(nn, v.f);
            dep[t] = d * inv;
            leaf[t] = static_cast<float>(nl) * inv;
        }
    }
    // Carry covered values into the empty texels, so filtering never reads
    // a black or flat-on texel at the outline.
    std::vector<uint8_t> have(cov.size());
    for (size_t t = 0; t < cov.size(); ++t) have[t] = cov[t] > 0.0f;
    for (int pass = 0; pass < 6; ++pass) {
        std::vector<uint8_t> next = have;
        for (int y = 0; y < R; ++y) {
            for (int x = 0; x < R; ++x) {
                const size_t t = static_cast<size_t>(y) * R + x;
                if (have[t]) continue;
                int n = 0;
                Vec3 c{}, nn{};
                float d = 0.0f, l = 0.0f;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int xx = x + i, yy = y + j;
                        if (xx < 0 || yy < 0 || xx >= R || yy >= R) continue;
                        const size_t u = static_cast<size_t>(yy) * R + xx;
                        if (!have[u]) continue;
                        ++n;
                        c = c + col[u];
                        nn = nn + nrm[u];
                        d += dep[u];
                        l += leaf[u];
                    }
                }
                if (n == 0) continue;
                const float inv = 1.0f / static_cast<float>(n);
                col[t] = c * inv;
                nrm[t] = bromath::vnormOr(nn, v.f);
                dep[t] = d * inv;
                leaf[t] = l * inv;
                next[t] = 1;
            }
        }
        have.swap(next);
    }
    // The rest takes the cell's mean.
    {
        int n = 0;
        Vec3 c{};
        float l = 0.0f;
        for (size_t t = 0; t < cov.size(); ++t)
            if (cov[t] > 0.0f) { ++n; c = c + col[t]; l += leaf[t]; }
        const Vec3 mc = n ? c * (1.0f / static_cast<float>(n)) : Vec3{};
        const float ml = n ? l / static_cast<float>(n) : 1.0f;
        for (size_t t = 0; t < cov.size(); ++t)
            if (!have[t]) { col[t] = mc; nrm[t] = v.f; dep[t] = 0.0f; leaf[t] = ml; }
    }
    for (int y = 0; y < R; ++y) {
        for (int x = 0; x < R; ++x) {
            const size_t t = static_cast<size_t>(y) * R + x;
            const size_t o = (static_cast<size_t>(oy + y) * out.width + (ox + x)) * 4;
            out.color[o] = toSrgb(col[t].x);
            out.color[o + 1] = toSrgb(col[t].y);
            out.color[o + 2] = toSrgb(col[t].z);
            out.color[o + 3] = toByte(cov[t]);
            out.data[o] = toByte(bromath::vdot(nrm[t], v.right) * 0.5f + 0.5f);
            out.data[o + 1] = toByte(bromath::vdot(nrm[t], v.up) * 0.5f + 0.5f);
            out.data[o + 2] = toByte(dep[t] / v.half * 0.5f + 0.5f);
            out.data[o + 3] = toByte(leaf[t]);
        }
    }
}

template <typename Fn>
void parallelFor(size_t n, Fn&& fn) {
    const size_t threads = std::max<size_t>(1, std::min<size_t>(n, std::thread::hardware_concurrency()));
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
    };
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

}  // namespace

Vec3 forestImpostorViewDir(int i, int j, int n) {
    const float ex = (static_cast<float>(i) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
    const float ey = (static_cast<float>(j) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
    const float px = 0.5f * (ex + ey), pz = 0.5f * (ex - ey);
    const float y = 1.0f - std::fabs(px) - std::fabs(pz);
    return bromath::vnorm(Vec3{px, std::max(y, 0.0f), pz});
}

void forestImpostorBasis(const Vec3& f, Vec3& right, Vec3& up) {
    Vec3 r = bromath::vcross(Vec3{0.0f, 1.0f, 0.0f}, f);
    if (bromath::vlen2(r) < 1e-10f) r = {1.0f, 0.0f, 0.0f};
    right = bromath::vnorm(r);
    up = bromath::vcross(f, right);
}

ForestImpostorAtlas bakeForestImpostors(const ForestNode& node, int views, int texels, int maxSide) {
    ForestImpostorAtlas out;
    const auto& specimens = node.specimens();
    const bromesh::MeshData& leaf = node.mesh();
    if (specimens.empty() || views < 1 || texels < 4) return out;
    const int nSpec = static_cast<int>(specimens.size());
    const int blocksX = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(nSpec))));
    const int blocksY = (nSpec + blocksX - 1) / blocksX;
    int R = texels;
    while (R > 4 && std::max(blocksX, blocksY) * views * R > maxSide) R -= 4;
    out.views = views;
    out.texels = R;
    out.blocksX = blocksX;
    out.width = blocksX * views * R;
    out.height = blocksY * views * R;
    out.color.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    out.data.assign(out.color.size(), 0);
    out.spheres.assign(static_cast<size_t>(nSpec) * 4, 0.0f);
    out.shapes.assign(static_cast<size_t>(nSpec) * 4, 0.0f);

    std::vector<Prepared> prepared(specimens.size());
    parallelFor(specimens.size(), [&](size_t s) { prepared[s] = prepare(node, specimens[s]); });
    for (int s = 0; s < nSpec; ++s) {
        out.spheres[s * 4] = prepared[s].center.x;
        out.spheres[s * 4 + 1] = prepared[s].center.y;
        out.spheres[s * 4 + 2] = prepared[s].center.z;
        out.spheres[s * 4 + 3] = prepared[s].radius;
        out.shapes[s * 4] = prepared[s].across;
        out.shapes[s * 4 + 1] = prepared[s].upDown;
    }

    const float* bc = node.barkColor();
    const Vec3 barkColor{bc[0], bc[1], bc[2]};
    const float leafLength = node.leafLength();
    const size_t cells = static_cast<size_t>(nSpec) * views * views;
    parallelFor(cells, [&](size_t job) {
        const int s = static_cast<int>(job / (views * views));
        const int cell = static_cast<int>(job % (views * views));
        const int i = cell % views, j = cell / views;
        View v;
        v.f = forestImpostorViewDir(i, j, views);
        forestImpostorBasis(v.f, v.right, v.up);
        v.center = prepared[s].center;
        v.half = prepared[s].radius;
        v.S = R * kSuper;
        Raster r;
        r.reset(v.S);
        drawLeaves(prepared[s], leaf, v, r);
        drawBark(prepared[s], v, barkColor, r);
        const int bx = s % blocksX, by = s / blocksX;
        pack(r, v, R, leafLength, out, (bx * views + i) * R, (by * views + j) * R);
    });
    return out;
}

}  // namespace bro::scene
