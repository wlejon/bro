#pragma once

// One frame's shadow plan: which lights cast shadows, the atlas tiles they
// take and each tile's projection, and which tiles must be re-rendered this
// frame (the rest still hold valid depth from an earlier one). Built by
// SceneRenderer's planner (scene_renderer_shadow.cpp); drawn by the Vulkan
// shadow pass and sampled by every lit shader (vulkan/pass_shadow.h,
// vulkan/scene_lighting.h).

#include <bromath/frustum.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

namespace bro::scene {

class LightNode;

struct ShadowTilePlan {
    /// World -> light clip, in bromath's convention (y up, [0,1] depth,
    /// never reversed — scene/depth_policy.h): caster culling and the tile
    /// cache's key.
    bromath::Mat4 viewProj;
    bromath::Frustum frustum;    // culling planes of viewProj
    /// The same mapping for positions relative to ShadowPlan::origin, what
    /// the GPU draws and samples with (camera-relative rendering,
    /// vulkan/scene_view.h). Built from the relative positions directly, so
    /// it keeps its precision however far the scene is from the origin.
    bromath::Mat4 relViewProj;
    float rect[4] = {};          // atlas uv: origin.xy, size.zw
    float bias = 0.0f;           // the light's constant depth bias
    float normalBias = 0.0f;     // the light's normal offset, world units
    // World size of one texel at the receiver: constant for an ortho tile,
    // per metre of light distance for a perspective one.
    float texelConst = 0.0f;
    float texelPerMetre = 0.0f;
    float zNear = 0.0f, zFar = 1.0f;
    bool ortho = true;
    bool render = false;         // re-render this frame (not cached)
    /// The nearest camera distance whose shading reads this tile: a sun's
    /// cascade from where the cascade before hands over to it
    /// (scene_lighting.glsl's SHADOW_CASCADE_BLEND), 0 for its first and for
    /// any other light's tile.
    float receiversFrom = 0.0f;
    const LightNode* light = nullptr;
    /// A sun's last cascade (of three or more), drawn every other frame
    /// (SceneRenderer::planShadowTiles): its light direction, the light-space
    /// eye it was fit about (x, y in the light's fixed basis, then depth
    /// along the light), its window's centre relative to that, and the
    /// sphere its receivers lie in (centre relative to the same eye).
    bool farCascade = false;
    bromath::Vec3 lightDir{0.0f, 0.0f, 0.0f};
    double lightEye[3] = {};
    float windowCentre[2] = {};
    float windowRadius = 0.0f;
    float fitCentre[3] = {};
    float fitRadius = 0.0f;
};

struct ShadowPlan {
    static constexpr int kMaxTiles = 16;
    static constexpr int kMaxLights = 32;

    int tileCount = 0;
    int heldTiles = 0;           // tiles standing in with the frame before's depth (planShadowTiles)
    bromath::Vec3 origin{0.0f, 0.0f, 0.0f};   // the camera eye relViewProj is relative to
    int atlasSize = 0;           // side of the square depth atlas, in texels
    int pcfTaps = 3;             // receiver PCF grid side: 1, 3 or 5
    ShadowTilePlan tiles[kMaxTiles];

    // Per light, indexed like SceneRenderer::frameLights(): the first tile
    // (-1 when unshadowed), how many (cascades, or 6 cube faces), and the
    // cascade far distances (view space) of a directional light.
    int lightSlot[kMaxLights] = {};
    int lightSlotCount[kMaxLights] = {};
    float cascadeSplit[kMaxLights][4] = {};
};

}  // namespace bro::scene
