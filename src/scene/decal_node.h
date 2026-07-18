#pragma once

#include "scene/scene_node.h"
#include <glad/gl.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

/// Projected decal node (Godot Decal analog). The decal volume is the unit
/// box [-0.5, 0.5]^3 in LOCAL space — the node's scale IS the box size (a
/// node with scale (2, 4, 2) projects over a 2x2 world-unit footprint through
/// 4 units along the projection axis), so tweening `scale` animates the decal
/// extent like any other node and there is no separate size property.
///
/// Projection runs along the node's local -Y (top-down by default, like
/// Godot); rotating the node reorients the projection. Texture U maps local
/// +X and V maps local +Z, as seen looking down the projection axis.
///
/// Rendered screen-space in the forward pipeline: after the opaque passes,
/// each decal draws its box, reconstructs the opaque scene position per
/// fragment from the scene depth snapshot, and alpha-blends
/// albedo * modulate onto the LIT HDR result (plus emission * strength).
/// This differs from Godot, which injects decals into material inputs
/// BEFORE lighting — see docs/scene-api.js for the honest comparison.
/// Decals only appear on depth-writing (opaque) geometry: translucent
/// meshes, splats, particles and billboards never receive them.
class DecalNode : public SceneNode {
public:
    explicit DecalNode(const std::string& name = "");
    ~DecalNode() override;

    Type type() const override { return Type::Decal; }

    // --- Modulate (albedo tint x master opacity) ---
    void setModulate(float r, float g, float b, float a) {
        modulate_[0] = r; modulate_[1] = g; modulate_[2] = b; modulate_[3] = a;
    }
    const float* modulate() const { return modulate_; }

    // --- Emission ---
    /// Multiplier on the emission texture (HDR — values > 1 bloom).
    void setEmissionStrength(float s) { emissionStrength_ = s < 0.0f ? 0.0f : s; }
    float emissionStrength() const { return emissionStrength_; }

    // --- Fades along the projection axis (local Y) ---
    // Godot-style falloff exponents. 0 (default) disables. When > 0, alpha
    // multiplies by pow(1 - t, fade) where t ramps 0 -> 1 from the volume
    // CENTER to the +Y end (upperFade) or the -Y end (lowerFade) — so the
    // decal is always full-strength at its center plane and fades toward
    // the ends; larger exponents fade earlier/harder.
    void setUpperFade(float f) { upperFade_ = f < 0.0f ? 0.0f : f; }
    float upperFade() const { return upperFade_; }
    void setLowerFade(float f) { lowerFade_ = f < 0.0f ? 0.0f : f; }
    float lowerFade() const { return lowerFade_; }

    // --- Normal fade ---
    /// Skip surfaces facing away from the projection direction. 0 (default)
    /// disables. When > 0, alpha multiplies by
    /// smoothstep(normalFade, 1, dot(N, projectionUp) * 0.5 + 0.5) — the
    /// same curve Godot uses — where N is a screen-space normal
    /// reconstructed from depth derivatives. Quality tradeoff: the
    /// reconstructed normal is exact on planar surfaces but faceted on
    /// curved ones and noisy at depth discontinuities (silhouette edges),
    /// so expect a 1px fringe there instead of Godot's clean G-buffer
    /// normal test.
    void setNormalFade(float f) {
        normalFade_ = f < 0.0f ? 0.0f : (f > 0.99f ? 0.99f : f);
    }
    float normalFade() const { return normalFade_; }

    // --- Draw order among decals ---
    /// Decals draw sorted by ascending priority (higher priority = drawn
    /// later = on top of lower-priority overlapping decals). Equal
    /// priorities keep tree order.
    void setRenderPriority(int p) { renderPriority_ = p; }
    int renderPriority() const { return renderPriority_; }

    // --- Textures (staged upload, same pattern as MeshNode) ---
    // Setters copy the RGBA8 bytes (tightly packed, top-left origin) and may
    // run without a GL context; flushPendingTextures() uploads on the GL
    // thread before the decal pass reads texture ids. A decal without an
    // albedo texture projects plain `modulate` (a colored box projection).

    void setAlbedoTexture(int width, int height, const uint8_t* rgba);
    void clearAlbedoTexture();
    bool hasAlbedoTexture() const { return hasAlbedo_; }
    GLuint albedoTextureId() const { return albedoTex_; }

    void setEmissionTexture(int width, int height, const uint8_t* rgba);
    void clearEmissionTexture();
    bool hasEmissionTexture() const { return hasEmission_; }
    GLuint emissionTextureId() const { return emissionTex_; }

    /// Upload/release any dirty staged texture slots. GL thread only.
    void flushPendingTextures();

    /// Release GPU resources (GL thread; called from the destructor like
    /// SpriteNode — nodes are destroyed on the GL thread).
    void releaseGL();

private:
    struct PendingTex {
        std::vector<uint8_t> data;
        int w = 0;
        int h = 0;
        bool dirty = false;
    };
    static void flushSlot(PendingTex& slot, GLuint& tex);

    float modulate_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissionStrength_ = 1.0f;
    float upperFade_ = 0.0f;
    float lowerFade_ = 0.0f;
    float normalFade_ = 0.0f;
    int renderPriority_ = 0;

    // True from the setter on (staged or uploaded); false after clear.
    bool hasAlbedo_ = false;
    bool hasEmission_ = false;

    GLuint albedoTex_ = 0;
    GLuint emissionTex_ = 0;
    PendingTex pendingAlbedo_;
    PendingTex pendingEmission_;
};

} // namespace bro::scene
