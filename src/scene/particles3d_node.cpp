#include "scene/particles3d_node.h"

#include "broimage/decode.h"

#include <bromath/rng.h>

#include <algorithm>
#include <cmath>

namespace bro::scene {

using bromath::Vec3;
using bromath::Color;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// center + (rand*2-1) * spread/2 — matches the 2D ParticleNode convention
// (spread is the full width of the range).
inline float randSpread(uint64_t& rng, float center, float spread) {
    return center + bromath::randSigned(rng) * (spread * 0.5f);
}

// Orthonormal basis perpendicular to a unit axis (Duff et al. branchless form).
inline void basisFromAxis(const Vec3& n, Vec3& t, Vec3& b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float xy = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * xy, -sign * n.x};
    b = {xy, sign + n.y * n.y * a, -n.y};
}

// Uniform direction on the spherical cap of half-angle `halfAngleRad`
// around unit `axis`.
inline Vec3 sampleCone(uint64_t& rng, const Vec3& axis, float halfAngleRad) {
    if (halfAngleRad <= 0.0f) return axis;
    float cosMax = std::cos(std::min(halfAngleRad, kPi));
    float z = cosMax + (1.0f - cosMax) * bromath::randFloat01(rng);
    float phi = bromath::randFloat01(rng) * 2.0f * kPi;
    float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    Vec3 t, b;
    basisFromAxis(axis, t, b);
    return t * (s * std::cos(phi)) + b * (s * std::sin(phi)) + axis * z;
}

// Rotate a direction by the 3x3 part of a world matrix (renormalized so
// non-uniform parent scale can't stretch launch speeds).
inline Vec3 rotateByMatrix(const bromath::Mat4& m, const Vec3& d) {
    Vec3 r{
        m.at(0, 0) * d.x + m.at(0, 1) * d.y + m.at(0, 2) * d.z,
        m.at(1, 0) * d.x + m.at(1, 1) * d.y + m.at(1, 2) * d.z,
        m.at(2, 0) * d.x + m.at(2, 1) * d.y + m.at(2, 2) * d.z,
    };
    float len = bromath::vlen(r);
    return len > 1e-8f ? r * (1.0f / len) : d;
}

inline Vec3 transformPoint(const bromath::Mat4& m, const Vec3& p) {
    return {
        m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
        m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
        m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3),
    };
}

} // namespace

Particles3DNode::Particles3DNode(const std::string& name) : SceneNode(name) {
    setMaxParticles(256);
    bounds_ = bromath::aempty3();
}

void Particles3DNode::setMaxParticles(int n) {
    if (n < 1) n = 1;
    particles_.assign(static_cast<size_t>(n), Particle{});
    liveCount_ = 0;
    searchHead_ = 0;
    boundsValid_ = false;
}

void Particles3DNode::setTexturePath(const std::string& path) {
    if (path == texPath_) return;
    texPath_ = path;
    texTried_ = false;
    texture_.clear();   // a new path decodes afresh rather than keeping the old image
}

void Particles3DNode::setSheet(int cols, int rows, int frames) {
    sheetCols_ = cols < 1 ? 1 : cols;
    sheetRows_ = rows < 1 ? 1 : rows;
    int total = sheetCols_ * sheetRows_;
    sheetFrames_ = (frames > 0 && frames < total) ? frames : total;
}

void Particles3DNode::setDirection(const Vec3& dir, float spreadDeg) {
    float len = bromath::vlen(dir);
    direction_ = len > 1e-8f ? dir * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
    spreadDeg_ = spreadDeg;
}

void Particles3DNode::setColorStops(std::vector<std::pair<float, Color>> stops) {
    if (stops.empty()) return;
    colorStops_ = std::move(stops);
}

void Particles3DNode::play() {
    playing_ = true;
    emitClock_ = 0.0f;
    finishedFired_ = false;
}

void Particles3DNode::clear() {
    for (auto& p : particles_) p.alive = false;
    liveCount_ = 0;
    searchHead_ = 0;
    emitAccum_ = 0.0f;
    boundsValid_ = false;
}

void Particles3DNode::burst(int n) {
    for (int i = 0; i < n; ++i) emitOne();
}

void Particles3DNode::emit(int n, const EmitOverride& o) {
    for (int i = 0; i < n; ++i) emitOne(&o);
}

void Particles3DNode::sampleEmitter(Vec3& outPos, Vec3& outDir) {
    switch (shape_) {
    case EmitterShape::Point:
        outPos = {0, 0, 0};
        outDir = sampleCone(rng_, direction_, spreadDeg_ * 0.5f * kPi / 180.0f);
        break;
    case EmitterShape::Sphere:
    case EmitterShape::Hemisphere: {
        Vec3 p = bromath::randInUnitSphere(rng_);
        if (shape_ == EmitterShape::Hemisphere && p.y < 0.0f) p.y = -p.y;
        outPos = p * shapeRadius_;
        // Radial launch; degenerate center falls back to the config direction.
        float len = bromath::vlen(p);
        Vec3 radial = len > 1e-6f ? p * (1.0f / len) : direction_;
        outDir = sampleCone(rng_, radial, spreadDeg_ * 0.5f * kPi / 180.0f);
        break;
    }
    case EmitterShape::Box:
        outPos = {bromath::randSigned(rng_) * shapeExtents_.x,
                  bromath::randSigned(rng_) * shapeExtents_.y,
                  bromath::randSigned(rng_) * shapeExtents_.z};
        outDir = sampleCone(rng_, direction_, spreadDeg_ * 0.5f * kPi / 180.0f);
        break;
    case EmitterShape::Cone: {
        // Spawn on a disc perpendicular to the axis; tilt outward toward the
        // rim so the stream diverges like a real cone nozzle.
        float phi = bromath::randFloat01(rng_) * 2.0f * kPi;
        float rNorm = std::sqrt(bromath::randFloat01(rng_));
        Vec3 t, b;
        basisFromAxis(direction_, t, b);
        Vec3 radial = t * std::cos(phi) + b * std::sin(phi);
        outPos = radial * (rNorm * shapeRadius_);
        float tilt = coneAngleDeg_ * kPi / 180.0f * rNorm;
        Vec3 dir = direction_ * std::cos(tilt) + radial * std::sin(tilt);
        outDir = sampleCone(rng_, dir, spreadDeg_ * 0.5f * kPi / 180.0f);
        break;
    }
    }
}

void Particles3DNode::emitOne(const EmitOverride* o) {
    if (liveCount_ >= static_cast<int>(particles_.size())) return;

    int n = static_cast<int>(particles_.size());
    int slot = -1;
    for (int i = 0; i < n; ++i) {
        int idx = (searchHead_ + i) % n;
        if (!particles_[idx].alive) { slot = idx; break; }
    }
    if (slot < 0) return;
    searchHead_ = (slot + 1) % n;

    Vec3 pos, dir;
    float speed;
    if (o) {
        // Sample the shape with the override's cone in place of the
        // configured one, then place it at the override position.
        const Vec3 keepDir = direction_;
        const float keepSpread = spreadDeg_;
        float len = bromath::vlen(o->direction);
        direction_ = len > 1e-8f ? o->direction * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
        spreadDeg_ = o->spreadDeg;
        sampleEmitter(pos, dir);
        direction_ = keepDir;
        spreadDeg_ = keepSpread;
        pos = pos + o->position;
        speed = randSpread(rng_, o->speed, o->speedSpread);
    } else {
        sampleEmitter(pos, dir);
        speed = randSpread(rng_, speed_, speedSpread_);
        if (space_ == SimSpace::World) {
            const auto& wm = worldMatrix();
            pos = transformPoint(wm, pos);
            dir = rotateByMatrix(wm, dir);
        }
    }

    Particle& p = particles_[slot];
    p.alive = true;
    p.pos = pos;
    p.vel = dir * speed;
    p.maxLife = lifeMin_;
    if (lifeMax_ > lifeMin_) p.maxLife += (lifeMax_ - lifeMin_) * bromath::randFloat01(rng_);
    if (o) p.maxLife *= o->lifeScale;
    if (p.maxLife < 1e-4f) p.maxLife = 1e-4f;
    p.sizeScale = o ? o->sizeScale : 1.0f;
    p.tint = o ? o->tint : Color{1.0f, 1.0f, 1.0f, 1.0f};
    if (liveCount_ == 0) maxSizeScale_ = 1.0f;
    if (p.sizeScale > maxSizeScale_) maxSizeScale_ = p.sizeScale;
    p.life = p.maxLife;
    p.rot = rotStartDeg_ * kPi / 180.0f;
    p.spin = randSpread(rng_, spinSpeedDeg_, spinSpreadDeg_) * kPi / 180.0f;
    ++liveCount_;

    // Grow the bounds immediately so a burst is cullable before its first tick.
    if (!boundsValid_) { bounds_ = bromath::aempty3(); boundsValid_ = true; }
    bounds_ = bromath::aexpand(bounds_, p.pos);
}

void Particles3DNode::onTick(float dtSec) {
    if (dtSec <= 0.0f || paused_) return;

    // Duration window: advance the emission clock, then emit by rate while
    // the window (or an endless system) is active.
    if (playing_ && duration_ > 0.0f) {
        emitClock_ += dtSec;
        if (loop_) {
            while (emitClock_ >= duration_) emitClock_ -= duration_;
        }
    }
    if (emissionActive() && rate_ > 0.0f) {
        emitAccum_ += rate_ * dtSec;
        while (emitAccum_ >= 1.0f) {
            emitAccum_ -= 1.0f;
            emitOne();
        }
    }

    // Gravity in sim space: world-space gravity for Local sims must be pulled
    // into emitter space, else a rotated emitter would bend "down" sideways.
    Vec3 g = gravity_;
    if (space_ == SimSpace::Local &&
        (g.x != 0.0f || g.y != 0.0f || g.z != 0.0f)) {
        // Inverse rotation = transpose for the (assumed) orthonormal part.
        const auto& wm = worldMatrix();
        g = {wm.at(0, 0) * gravity_.x + wm.at(1, 0) * gravity_.y + wm.at(2, 0) * gravity_.z,
             wm.at(0, 1) * gravity_.x + wm.at(1, 1) * gravity_.y + wm.at(2, 1) * gravity_.z,
             wm.at(0, 2) * gravity_.x + wm.at(1, 2) * gravity_.y + wm.at(2, 2) * gravity_.z};
    }

    bounds_ = bromath::aempty3();
    boundsValid_ = false;
    float dragFactor = (drag_ != 1.0f) ? std::pow(drag_, dtSec) : 1.0f;

    for (auto& p : particles_) {
        if (!p.alive) continue;
        p.life -= dtSec;
        if (p.life <= 0.0f) {
            p.alive = false;
            --liveCount_;
            continue;
        }
        if (dragFactor != 1.0f) p.vel = p.vel * dragFactor;
        p.vel = p.vel + g * dtSec;
        p.pos = p.pos + p.vel * dtSec;
        p.rot += p.spin * dtSec;

        if (!boundsValid_) { boundsValid_ = true; bounds_ = {p.pos, p.pos}; }
        else bounds_ = bromath::aexpand(bounds_, p.pos);
    }

    // One-shot completion: emission window over and every particle expired.
    if (playing_ && duration_ > 0.0f && !loop_ && !finishedFired_ &&
        emitClock_ >= duration_ && liveCount_ == 0) {
        finishedFired_ = true;
        finishedPending_ = true;
        playing_ = false;
    }
}

bool Particles3DNode::worldBounds(bromath::AABB3& out) const {
    if (!boundsValid_ || liveCount_ <= 0) return false;
    float pad = 0.5f * maxSizeScale_ * std::max(std::fabs(sizeStart_), std::fabs(sizeEnd_));
    bromath::AABB3 b = bounds_;
    b.min = b.min - Vec3{pad, pad, pad};
    b.max = b.max + Vec3{pad, pad, pad};
    if (space_ == SimSpace::World) {
        out = b;
        return true;
    }
    // Local sim: transform the 8 corners into world space.
    const auto& wm = worldMatrix();
    bromath::AABB3 w = bromath::aempty3();
    for (int i = 0; i < 8; ++i) {
        Vec3 c{(i & 1) ? b.max.x : b.min.x,
               (i & 2) ? b.max.y : b.min.y,
               (i & 4) ? b.max.z : b.min.z};
        w = bromath::aexpand(w, transformPoint(wm, c));
    }
    out = w;
    return true;
}

Color Particles3DNode::evalColor(float u) const {
    const auto& stops = colorStops_;
    if (stops.size() == 1 || u <= stops.front().first) return stops.front().second;
    if (u >= stops.back().first) return stops.back().second;
    for (size_t i = 1; i < stops.size(); ++i) {
        if (u <= stops[i].first) {
            float span = stops[i].first - stops[i - 1].first;
            float t = span > 1e-6f ? (u - stops[i - 1].first) / span : 1.0f;
            return bromath::clerp(stops[i - 1].second, stops[i].second, t);
        }
    }
    return stops.back().second;
}

bool Particles3DNode::ensureTextureLoaded() {
    if (!texture_.empty()) return true;
    if (texTried_ || texPath_.empty()) return false;
    texTried_ = true;
    broimage::Image img;
    if (!broimage::decode_file(texPath_, img) || img.width <= 0 || img.height <= 0) {
        return false;
    }
    texture_.adopt(img.width, img.height, std::move(img.pixels));
    return true;
}

const std::vector<float>& Particles3DNode::buildInstanceData(const Vec3& camFwd) {
    drawOrder_.clear();
    const int n = static_cast<int>(particles_.size());
    for (int i = 0; i < n; ++i) {
        if (particles_[i].alive) drawOrder_.push_back(static_cast<uint32_t>(i));
    }
    if (drawOrder_.empty()) {
        instanceData_.clear();
        return instanceData_;
    }

    // Normal blend is order-dependent: sort back-to-front by view depth.
    // Additive is commutative — skip the sort.
    if (blend_ == Blend::Normal && drawOrder_.size() > 1) {
        depthKey_.resize(particles_.size());
        const bool local = (space_ == SimSpace::Local);
        const auto& wm = worldMatrix();
        for (uint32_t idx : drawOrder_) {
            Vec3 wp = local ? transformPoint(wm, particles_[idx].pos)
                            : particles_[idx].pos;
            depthKey_[idx] = bromath::vdot(wp, camFwd);
        }
        std::sort(drawOrder_.begin(), drawOrder_.end(),
                  [this](uint32_t a, uint32_t b) { return depthKey_[a] > depthKey_[b]; });
    }

    // Fill the instance stream: pos(3) size(1) rgba(4) rot(1) frame(1).
    const int frames = (sheetCols_ > 1 || sheetRows_ > 1) ? sheetFrames_ : 1;
    instanceData_.resize(drawOrder_.size() * kInstFloats);
    float* out = instanceData_.data();
    for (uint32_t idx : drawOrder_) {
        const Particle& p = particles_[idx];
        float u = 1.0f - p.life / p.maxLife;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        out[0] = p.pos.x;
        out[1] = p.pos.y;
        out[2] = p.pos.z;
        out[3] = (sizeStart_ + (sizeEnd_ - sizeStart_) * u) * p.sizeScale;
        Color c = evalColor(u);
        out[4] = c.r * p.tint.r;
        out[5] = c.g * p.tint.g;
        out[6] = c.b * p.tint.b;
        out[7] = c.a * p.tint.a;
        out[8] = p.rot;
        float f = u * static_cast<float>(frames);
        out[9] = f >= static_cast<float>(frames) ? static_cast<float>(frames - 1) : f;
        out += kInstFloats;
    }
    return instanceData_;
}

} // namespace bro::scene
