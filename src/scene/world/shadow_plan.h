#pragma once

// One frame's shadow plan: the virtual maps every shadowed light samples
// (the sun's clipmap levels, a spot's map, a point light's six faces), the
// page table that finds each map's resident pages in the physical pool, and
// what the frame draws: the rects of pages it redraws (views), each into the
// scratch target, and the copies between the scratch and the pool. Built by
// SceneRenderer's planner (scene_renderer_shadow.cpp) over the paging policy
// (world/shadow_pages.h); drawn by the Vulkan shadow pass
// (vulkan/shadow/pass_shadow.h), marked by the marking pass
// (vulkan/shadow/pass_shadow_mark.h) and sampled by every lit shader
// (common/scene_shadow_pages.glsl).

#include "scene/vulkan/shaders/shadow/shadow_pages_layout.h"

#include <bromath/frustum.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

class LightNode;

/// A virtual map as sampled this frame.
struct ShadowMapPlan {
    /// Positions relative to ShadowPlan::origin -> clip (bromath's y-up
    /// convention, [0,1] depth, never reversed) over the whole map: a
    /// clipmap level's window, a local map's face. Vulkan's clip of it is
    /// the map's (u, v, depth) once its x, y are taken to [0, 1].
    bromath::Mat4 relViewProj;
    bromath::Mat4 viewProj;      // the same for absolute positions (culling)
    int kind = SP_KIND_CLIPMAP;
    int light = -1;              // index in SceneRenderer::frameLights()
    int level = 0;               // a clipmap level's
    int firstEntry = 0;          // its run of the page table
    int originX = 0, originY = 0;   // a clipmap window's origin page, mod SP_WINDOW
    float texel = 0.0f;          // world size of a texel: a clipmap level's (m)
    float texelPerMetre = 0.0f;  // a local map's finest mip's, a metre from the light
    float zNear = 0.0f, zFar = 1.0f;
    bool ortho = true;
    float bias = 0.0f;           // the light's constant depth bias
    float normalBias = 0.0f;     // the light's normal offset (m)
};

/// Which of the plants a shadow view draws. A crown sways in its shadow only
/// where the sun's penumbra under it is narrower than its sway shows
/// through: the sun's clipmap draws the plants into the sway level's
/// overlays (Swaying: the crowns whose penumbra is under
/// ShadowPlan::stillPenumbra), the rest (Still) into the cached pages of
/// the sway level and finer, and all of them (All) into the cached pages
/// past it, where sway hides under the filter. The lit shaders read a wide
/// penumbra's shadow from the level it spans, past the sway level, so the
/// Still crowns of the finer pages, drawn as coarse as their penumbra,
/// serve only where a receiver lies nearer its caster than the crown's
/// height. A local light's overlays take them all.
enum class ShadowPlants : uint8_t { None, Swaying, Still, All };

/// A rect of a map's pages drawn this frame into the scratch target, its
/// pages and their border.
struct ShadowViewPlan {
    int map = 0;
    int mip = 0;                 // a local map's mip
    int x = 0, y = 0, w = 0, h = 0;   // pages, in the map's (mip's) grid of (u, v)
    bool dynamic = false;        // an overlay: what moves alone, sampled in place over the pages' cached depth
    ShadowPlants plants = ShadowPlants::None;   // the plants' casters that go in
    float penumbra = 0.0f;       // a sun view's penumbra a metre above the receiver (m): parts Swaying from Still
    bool grass = false;          // the grass's casters go in (the near levels only)
    int scratchX = 0, scratchY = 0;   // the bordered rect's first texel in the scratch target
    int texelsW = 0, texelsH = 0;     // its size
    bromath::Mat4 relViewProj;   // relative to ShadowPlan::origin, cropped to the bordered rect
    bromath::Frustum frustum;    // of the absolute view-projection: caster culling
    float texel = 0.0f;          // an ortho view's texel (m)
    float texelPerMetre = 0.0f;  // a perspective view's, a metre from the light
    bool ortho = true;
};

/// How a caster casts: still (in the pages' cached depth), moving (drawn
/// each frame into the overlays over it), swaying (in the sway level's
/// overlays, which it and the finer levels read, and the cached depth of
/// the coarser levels, where its sway hides under the filter; the plants
/// part their crowns by penumbra themselves, ShadowPlants), or as the grass
/// (the sway level's overlays alone).
enum class ShadowMotion : uint8_t { Still, Moving, Swaying, Grass };

/// Whether a caster of `motion` draws into view `v`.
inline bool shadowCastsInto(const ShadowViewPlan& v, ShadowMotion motion) {
    switch (motion) {
    case ShadowMotion::Still: return !v.dynamic;
    case ShadowMotion::Moving: return v.dynamic;
    case ShadowMotion::Swaying: return v.plants == ShadowPlants::Swaying || v.plants == ShadowPlants::All;
    case ShadowMotion::Grass: return v.grass;
    }
    return false;
}

/// A page redrawn this frame, copied from the scratch target into the pool,
/// SP_STRIDE texels square (the page and its border).
struct ShadowPageCopy {
    int scratchX = 0, scratchY = 0;   // the bordered page's first texel in the scratch target
    int physical = 0;                 // its page in the pool
    bool plants = false;              // drawn in a view every plant draws into (its job's kind)
};

struct ShadowPlan {
    static constexpr int kMaxLights = 32;

    bromath::Vec3 origin{0.0f, 0.0f, 0.0f};   // the camera eye relViewProj is relative to
    uint64_t serial = 0;         // the plan's (a marking pass's requests name it)
    int pcfTaps = 3;             // receiver PCF grid side: 1, 3 or 5

    int mapCount = 0;
    ShadowMapPlan maps[SP_MAX_MAPS];
    // Per light, indexed like SceneRenderer::frameLights(): its first map
    // (-1 when unshadowed), how many (the sun's levels, a point's 6 faces),
    // and how far from the eye its shadow reaches (m; 0 none).
    int lightMap[kMaxLights] = {};
    int lightMapCount[kMaxLights] = {};
    float lightReach[kMaxLights] = {};
    // The sun's next clipmap while it is built (ShadowPages::building()):
    // its levels, marked and drawn, not sampled until it is shown.
    int nextCount = 0;
    ShadowMapPlan next[SP_MAX_LEVELS];
    float finestTexel = 0.0f;    // the sun's level 0 texel (m)
    int levels = 0;              // the sun's clipmap levels
    int swayLevel = 0;           // the level whose overlays the finer ones read (ShadowPages::swayLevel)
    int searchLevel = 0;         // where the lit shaders look for wide penumbrae (ShadowPages::searchLevel)
    float stillPenumbra = 0.0f;  // ShadowPagesConfig's: a shadow blurred wider hides its caster's sway
    // A screen pixel's footprint, which picks the level (or mip) a surface
    // samples and the marking pass marks: metres a pixel a metre from the
    // eye (a perspective camera), or metres a pixel (an orthographic one).
    float pixelAngle = 0.0f, orthoPixel = 0.0f;

    // The page table, two words an entry (a sampled map's run each): its
    // cached page (SP_RESIDENT | physical) and its overlay this frame
    // (SP_RESIDENT | the page's first texel in the scratch target,
    // SP_OVERLAY_X/Y), and the pool's size in pages. The entries the
    // marking pass may name run on past it (the next clipmap's).
    std::vector<uint32_t> table;
    int entryCount = 0;
    int poolColumns = 0, poolRows = 0;
    int scratchSize = 0;         // the scratch target's side, texels

    // What the frame draws: the overlays first, then the static views,
    // whose pages are copied out to the pool (copyOut).
    int viewCount = 0;
    ShadowViewPlan views[SP_MAX_VIEWS];
    std::vector<ShadowPageCopy> copyOut;
    uint32_t staticPages = 0;    // pages the static views redraw (the world service's jobs)
    uint32_t plantPages = 0;     // of them, in views every plant draws into (ShadowPlants::All)

    bool any() const { return mapCount > 0; }
};

}  // namespace bro::scene
