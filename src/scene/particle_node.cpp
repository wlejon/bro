#include "scene/particle_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bro::scene {

namespace {

inline float rand01() {
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

inline float randSpread(float center, float spread) {
    return center + (rand01() * 2.0f - 1.0f) * (spread * 0.5f);
}

inline uint8_t lerpU8(uint8_t a, uint8_t b, float t) {
    float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}

constexpr float kPi = 3.14159265358979323846f;
constexpr int   kCanvasCompositePlus = 11;

} // namespace

ParticleNode::ParticleNode(const std::string& name) : SceneNode(name) {
    setMaxParticles(256);
}

void ParticleNode::setMaxParticles(int n) {
    if (n < 1) n = 1;
    particles_.assign(static_cast<size_t>(n), Particle{});
    liveCount_ = 0;
    searchHead_ = 0;
}

void ParticleNode::setTexturePath(const std::string& path) {
    texPath_ = path;
    texLoaded_ = false;
    texPixels_.clear();
    texW_ = texH_ = 0;
}

void ParticleNode::clear() {
    for (auto& p : particles_) p.alive = false;
    liveCount_ = 0;
    searchHead_ = 0;
    emitAccum_ = 0.0f;
}

void ParticleNode::burst(int n) {
    for (int i = 0; i < n; ++i) emitOne();
}

void ParticleNode::ensureTextureLoaded() {
    if (texLoaded_ || texPath_.empty()) return;
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(texPath_.c_str(), &w, &h, &channels, 4);
    if (data) {
        texW_ = w; texH_ = h;
        texPixels_.assign(data, data + w * h * 4);
        stbi_image_free(data);
    }
    texLoaded_ = true;
}

void ParticleNode::emitOne() {
    if (liveCount_ >= (int)particles_.size()) return;

    // Find a free slot. Linear search starting from searchHead_; the
    // hard cap keeps this bounded.
    int n = (int)particles_.size();
    int slot = -1;
    for (int i = 0; i < n; ++i) {
        int idx = (searchHead_ + i) % n;
        if (!particles_[idx].alive) { slot = idx; break; }
    }
    if (slot < 0) return;
    searchHead_ = (slot + 1) % n;

    Particle& p = particles_[slot];
    p.alive = true;
    p.x = 0.0f;
    p.y = 0.0f;
    float angRad   = randSpread(angle_, angleSpread_) * kPi / 180.0f;
    float speedVal = randSpread(speed_, speedSpread_);
    p.vx = std::cos(angRad) * speedVal;
    p.vy = std::sin(angRad) * speedVal;
    p.maxLife = lifeMin_;
    if (lifeMax_ > lifeMin_) {
        p.maxLife += (lifeMax_ - lifeMin_) * rand01();
    }
    p.life = p.maxLife;
    p.rot = rotStart_ * kPi / 180.0f;
    p.spin = randSpread(spinSpeed_, spinSpread_) * kPi / 180.0f;
    ++liveCount_;
}

void ParticleNode::onTick(float dtSec) {
    if (dtSec <= 0.0f) return;

    // Emit by rate.
    if (playing_ && rate_ > 0.0f) {
        emitAccum_ += rate_ * dtSec;
        while (emitAccum_ >= 1.0f) {
            emitAccum_ -= 1.0f;
            emitOne();
        }
    }

    // Integrate.
    for (auto& p : particles_) {
        if (!p.alive) continue;
        p.life -= dtSec;
        if (p.life <= 0.0f) {
            p.alive = false;
            --liveCount_;
            continue;
        }
        // Drag (per-second). Skip the pow if drag is unity to save cycles.
        if (drag_ != 1.0f) {
            float d = std::pow(drag_, dtSec);
            p.vx *= d;
            p.vy *= d;
        }
        p.vx += gravX_ * dtSec;
        p.vy += gravY_ * dtSec;
        p.x  += p.vx * dtSec;
        p.y  += p.vy * dtSec;
        p.rot += p.spin * dtSec;
    }
}

void ParticleNode::onRender(SceneGraph& graph) {
    if (liveCount_ <= 0) return;
    auto* cs = graph.canvasScene();
    if (!cs) return;

    ensureTextureLoaded();

    const auto& wm = worldMatrix();

    cs->save();
    // Particle simulation lives in node-local space; apply the node's world
    // transform once and draw each particle's offset relative to that.
    cs->setTransform(wm.m[0][0], wm.m[0][1], wm.m[1][0], wm.m[1][1], wm.m[3][0], wm.m[3][1]);

    int prevOp = cs->globalCompositeOperation();
    if (blend_ == Blend::Additive) {
        cs->setGlobalCompositeOperation(kCanvasCompositePlus);
    }

    bool useTex = !texPixels_.empty() && texW_ > 0 && texH_ > 0;

    for (const auto& p : particles_) {
        if (!p.alive) continue;
        float u = (p.maxLife > 0.0f) ? (1.0f - p.life / p.maxLife) : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;

        float size = sizeStart_ + (sizeEnd_ - sizeStart_) * u;
        if (size <= 0.0f) continue;

        Color col;
        col.r = lerpU8(colorStart_.r, colorEnd_.r, u);
        col.g = lerpU8(colorStart_.g, colorEnd_.g, u);
        col.b = lerpU8(colorStart_.b, colorEnd_.b, u);
        col.a = lerpU8(colorStart_.a, colorEnd_.a, u);
        if (col.a == 0) continue;

        cs->setGlobalAlpha(col.a / 255.0f);
        cs->setFillColor(col.r, col.g, col.b, 255);

        if (useTex) {
            // Translate + rotate via canvas transforms layered on top of the
            // emitter's world transform. Cheap save/restore per particle —
            // canvas state stack is shallow and not GPU-critical.
            cs->save();
            cs->translate(p.x, p.y);
            if (p.rot != 0.0f) cs->rotate(p.rot);
            float half = size * 0.5f;
            cs->drawImage(texPixels_.data(), texW_, texH_,
                          0.0f, 0.0f, (float)texW_, (float)texH_,
                          -half, -half, size, size);
            cs->restore();
        } else {
            // Filled circle path — cheaper than rotated quads when there's
            // no texture; rotation is invisible on a circle anyway.
            cs->beginPath();
            cs->arc(p.x, p.y, size * 0.5f, 0.0f, 2.0f * kPi, false);
            cs->fill();
        }
    }

    cs->setGlobalAlpha(1.0f);
    if (blend_ == Blend::Additive) {
        cs->setGlobalCompositeOperation(prevOp);
    }
    cs->restore();
}

} // namespace bro::scene
