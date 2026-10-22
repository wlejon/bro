#pragma once
// Cached shadow pages (docs/scene-world-architecture.md §8): the paging
// policy, the CPU half, testable without a GPU (bro_shadow_pages_test). The
// GPU half draws what it plans (vulkan/shadow/pass_shadow.h), marks the
// pages each frame's pixels want (vulkan/shadow/pass_shadow_mark.h) and
// samples them (common/scene_shadow_pages.glsl); the numbers they share are
// shaders/shadow/shadow_pages_layout.h's.
//
// Virtual maps. The sun's shadow is a clipmap (SP_MAX_LEVELS levels, level
// L's texel the finest texel times 2^L), each level a window of SP_WINDOW
// pages a side fixed in light space and snapped in whole pages about the
// eye: a camera move changes which pages are in the windows, never what a
// page holds, so a page drawn stays valid and its edges do not swim. Light
// space is the plane across the light (a, b) and the depth along it (c), in
// double, about an anchor: the eye where the clipmap was begun, its plane
// coordinates there kept from the clipmap before. A spot light's map and
// each of a point light's faces is a mip chain of SP_LOCAL_MIPS levels; a
// light that moves redraws its pages.
//
// The sun's turn. A page drawn under one direction is wrong under another
// everywhere at once, so pages are not redrawn one by one as the sun turns
// (each would show the new sun while its neighbours show the old). When the
// sun has turned sunTurn past the clipmap shown, or its casters outgrow its
// depth range, the next clipmap is begun (building()): a second set of
// levels in light space of the sun as it is then, marked by the marking
// pass, drawn by the world service from what the shown one leaves of the
// budget (its pages' error is under any shown page's), never sampled. When
// every page it needs is drawn (or buildFrames have passed) it is shown in
// one frame, the old one's pages freed: every shadow moves at once by the
// turn, as the sun moved. A turn while one is built waits for it.
//
// Pages. Each map's entries hold the page they stand for, its physical page
// in the pool (SP_STRIDE texels with its border) when it has one, whether it
// holds depth (drawn) and whether that depth is stale (dirty: a caster or
// the light moved since). The marking pass's requests, read back a frame or
// two late, name what the pixels need (needed); a needed page not drawn or
// dirty is a job for the world service (wants()), ranked by its error in
// pixels: what shows in its place is its coarsest drawn ancestor's depth
// (2^k pixels for an ancestor k levels up), a stale page's is a pixel's; a
// needed page with no needed ancestor is the top of its chain and required.
// A page the plants draw into (a sun page past the sway level) is a job of
// another kind than one they do not: biomes' cost a hundred times one of
// the near levels', where only what stands still casts.
// The world service's grants (grant()) are drawn the next frame, into
// physical pages from the free list, else the least recently needed, and
// sampled the frame they are drawn (copied into the pool ahead of the lit
// passes); a stale one keeps drawing until it is redrawn in place.
//
// What moves. A page holds what does not move. What moves every frame is
// drawn alone each frame into an overlay of each needed page its bounds
// touch (addDynamic): a page of the scratch target, cleared and sampled in
// place, its shadow multiplied into the cached page's. Overlays are drawn in
// the sun's levels from the sway level (swayLevel(), its texel about
// swayTexel) up; a finer level reads its sway-level ancestor's, since a
// leaf's shadow is softer than that texel (the sun's half degree across the
// metres to the ground), so what moves near the eye costs one level's pages,
// not the four or five finer ones' too. The plants and the grass are in the
// sway level's overlays alone and out of the cached depth of it and every
// finer level; coarser, where their sway hides under the filter, they hold
// still in the cached depth. What a caster's change invalidates is the
// pages its bounds (padded by a page's border) touch (invalidate()).
//
// What a frame draws (plan()): rects of pages (views) cut from the pages it
// redraws, each map's covered exactly while the frame's SP_MAX_VIEWS allow,
// coarser blocks where they do not, packed into the scratch target; the
// overlays first, then the world service's grants.

#include "scene/world/shadow_plan.h"

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>
#include <span>
#include <vector>

namespace bro::scene {

struct ShadowPagesConfig {
    float finestTexel = 1.0f / 1024.0f;   // the sun's level 0 texel (m)
    int levels = SP_MAX_LEVELS;           // the sun's clipmap levels
    int poolColumns = 60, poolRows = 60;  // the pool, pages a side
    int overlayPages = 512;               // the frame's overlays at most
    int staleNow = 128;                   // stale needed pages redrawn the frame they go stale, at most
    int scratchSize = 4096;               // the scratch target's side, texels
    float sunTurn = 0.0017f;              // radians the sun turns before the next clipmap is begun (~0.1 degree)
    int buildFrames = 90;                 // a next clipmap is shown after at most this many frames
    // The sway level's texel (m): the overlays of what moves start at the
    // level with about this texel, and the finer levels read them.
    float swayTexel = 0.008f;
    // A shadow the sun's penumbra blurs wider than this (m) hides its
    // caster's sway: the plants whose crowns cast it are cached at every
    // level (ShadowPlants), and the lit shaders filter it at the level of
    // its penumbra (ShadowPlan::searchLevel).
    float stillPenumbra = 0.5f;
};

/// The sun this frame.
struct ShadowSun {
    uint32_t lightId = 0;
    int lightIndex = 0;                   // in SceneRenderer::frameLights()
    bromath::Vec3 direction{0.0f, -1.0f, 0.0f};   // light -> scene
    bromath::AABB3 casters;               // every caster's bounds: the depth range
    float bias = 0.0f, normalBias = 0.0f;
    float reach = 0.0f;                   // how far from the eye it shadows (m; 0 none)
    float angularRadius = 0.0f;           // the sun disc's (radians): its penumbra
};

/// A local light this frame: a spot (one face) or a point light (six).
struct ShadowLocal {
    uint32_t lightId = 0;
    int lightIndex = 0;
    int faces = 1;
    bromath::Mat4 viewProj[6];            // absolute -> clip (bromath's), each face
    bromath::Mat4 relViewProj[6];         // relative to the frame's eye
    float texelPerMetre = 0.0f;           // the finest mip's texel a metre from the light
    float zNear = 0.1f, zFar = 1.0f;
    float bias = 0.0f, normalBias = 0.0f;
};

/// A page the world service may draw (WorldJob's fields).
struct ShadowPageWant {
    uint64_t key = 0;
    float error = 0.0f;
    double size = 0.0;
    bool required = false;
    bool plants = false;   // the plants' casters draw into it (a sun page past the sway level): the dearer kind
    /// Its cost in its kind's units: a page; a plants page its texels and
    /// the elements cast into it (addLoad), which cost about alike.
    double units = 1.0;
};

struct ShadowPagesStats {
    uint32_t needed = 0;        // pages the newest requests name
    uint32_t resident = 0;      // needed and sampled this frame
    uint32_t drawn = 0;         // pages the static views redraw
    uint32_t dynamic = 0;       // overlays drawn
    uint32_t wanted = 0;        // needed, not drawn or stale, left for later frames
    uint32_t evicted = 0;       // pages taken from another this frame
    uint32_t views = 0;
    uint32_t building = 0;      // the next clipmap's needed pages not drawn yet
};

class ShadowPages {
public:
    ShadowPages();
    void configure(const ShadowPagesConfig& config);
    const ShadowPagesConfig& config() const { return config_; }

    /// A frame begins about `eye`: its plan's serial is the next.
    void beginFrame(const bromath::Vec3& eye);
    /// This frame's shadowed lights: at most one sun, then local lights
    /// while the maps hold them (SP_MAX_MAPS less the sun's levels); false
    /// for a light there is no room for (it casts no shadow).
    void setSun(const ShadowSun& sun);
    bool addLocal(const ShadowLocal& local);
    /// The lights are set: a light not set this frame gives its pages back.
    void endLights();

    /// A caster changed inside `box`: the pages it touches are stale (from
    /// endLights() on, when the frame's maps are set: met before, it waits
    /// for them).
    void invalidate(const bromath::AABB3& box);
    void invalidateAll();
    /// Something moves inside `box` this frame: drawn into the overlays of
    /// the needed pages it touches, in every local map and the sun's levels
    /// from swayLevel() up (`sways`: that level alone).
    void addDynamic(const bromath::AABB3& box, bool sways = false);
    /// Casters inside `box` that draw into the sun's plants pages (the
    /// levels past swayLevel()), `elements[level]` of their elements at
    /// each: spread evenly over the pages of each level its plane rect
    /// touches, they price those pages (ShadowPageWant::units). A plants
    /// page's cost follows the elements drawn into it, not its count: one a
    /// forest's shadow falls on costs many a meadow's.
    void addLoad(const bromath::AABB3& box, const float (&elements)[SP_MAX_LEVELS]);
    /// What the camera sees this frame (its frustum's bounds). Its base
    /// pages, which cover where a caster's shadow can fall in it (across the
    /// light, it and the casters overlap) at the sun's finest level spanning
    /// that in a few pages, and a local map's coarsest mip, are always
    /// needed and drawn the frame they are missing or stale, not left to the
    /// world service: a pixel whose own page waits (a cut, the first frame)
    /// shows them rather than no shadow.
    void setSeen(const bromath::AABB3& box);
    /// The sun's level whose texel is about ShadowPagesConfig::swayTexel:
    /// its overlays carry what moves for it and every finer level.
    int swayLevel() const;
    /// The sun's level whose texel is about ShadowPagesConfig::stillPenumbra,
    /// past the sway level: where the lit shaders look for blockers whose
    /// penumbra is wider (a gather there spans about that penumbra).
    int searchLevel() const;

    /// The marking pass's requests for the plan `serial` named: a bit an
    /// entry (SP_LEVEL_ENTRIES a clipmap level, then the local maps', then
    /// the next clipmap's levels), set where a pixel needed it.
    void request(uint64_t serial, std::span<const uint32_t> bits);

    /// The frame's plan: its maps and page table, and what it draws (the
    /// overlays and the grants the world service made last frame).
    void plan(ShadowPlan& out);
    /// The pages wanted and not drawn this frame, for the world service.
    const std::vector<ShadowPageWant>& wants() const { return wants_; }
    /// The world service's grants of wants() (a call a kind of them, added
    /// together): drawn next frame.
    void grant(std::span<const uint64_t> keys);

    const ShadowPagesStats& stats() const { return stats_; }
    int entryCount() const { return static_cast<int>(entries_.size()); }
    uint64_t serial() const { return serial_; }

    // --- The paging's view, for tests and the stats ---
    /// The shown clipmap's (`next`: the one built) light-space plane
    /// coordinates (a, b) and depth (c) of `p`.
    void sunCoordinates(const bromath::Vec3& p, double& a, double& b, double& c, bool next = false) const;
    /// Clipmap level `level`'s window origin page (absolute).
    void window(int level, int64_t& x, int64_t& y) const;
    /// The page of `level` that holds the plane point (a, b).
    static int64_t pageOf(double coordinate, double pageSize);
    double pageSize(int level) const;
    /// The entry of the shown (`next`: the built) clipmap level `level`'s
    /// absolute page (x, y), -1 outside its window.
    int clipmapEntry(int level, int64_t x, int64_t y, bool next = false) const;
    /// A next clipmap is being built.
    bool building() const { return building_; }
    struct EntryView {
        int64_t x = 0, y = 0;
        int physical = -1;
        bool drawn = false, dirty = false, needed = false;
    };
    EntryView entry(int index) const;
    bool hasSun() const { return sun_.on; }

private:
    struct Entry {
        int64_t x = 0, y = 0;    // its page: a clipmap level's absolute plane page, a local mip's
        int32_t physical = -1;
        uint32_t overlay = 0;    // its overlay's table word this frame (0: none)
        uint64_t needed = 0;     // the newest plan serial whose requests named it
        uint64_t base = 0;       // the plan serial it was a base page of (setSeen)
        bool occupied = false;   // a clipmap slot: (x, y) is in its level's window
        bool drawn = false;
        bool dirty = false;
        bool fresh = false;      // first drawn this frame
        bool dynamic = false;    // overlaid this frame
    };
    struct Map {
        int kind = SP_KIND_CLIPMAP;
        int level = 0;           // clipmap level, or the local slot
        int first = 0;           // its entries
        int light = -1;
        uint32_t lightId = 0;
        int face = 0;
        bromath::Mat4 relViewProj, viewProj;
        float texel = 0.0f, texelPerMetre = 0.0f, zNear = 0.0f, zFar = 1.0f;
        bool ortho = true;
        float bias = 0.0f, normalBias = 0.0f;
        bool next = false;       // a level of the clipmap being built
    };
    struct Sun {
        bool on = false, had = false;
        uint32_t lightId = 0;
        int lightIndex = 0;
        double s[3] = {}, u[3] = {}, d[3] = {};   // the basis: across (a), up (-b), along (c)
        float penumbra = 0.0f;                    // its penumbra a metre above the receiver (ShadowViewPlan's)
        double anchor[3] = {};
        double offset[3] = {};                    // (a, b, c) at the anchor
        double zLo = 0.0, zHi = 0.0;              // the depth range (c)
        double casterRect[4] = {};                // the casters' (a0, a1, b0, b1) this frame
        bool haveRange = false;
        int64_t ox[SP_MAX_LEVELS] = {}, oy[SP_MAX_LEVELS] = {};
        bool haveWindows = false;
        float bias = 0.0f, normalBias = 0.0f, reach = 0.0f;
    };
    struct LocalSlot {
        bool used = false, seen = false;
        uint32_t lightId = 0;
        int face = 0;
        bromath::Mat4 viewProj;
    };
    static constexpr int kLocalSlots = SP_MAX_MAPS - SP_MAX_LEVELS;
    static constexpr double kBasePages = 4.0;   // the sun's base level spans what is seen in this many pages
    struct History {
        uint64_t serial = 0;
        int64_t ox[SP_MAX_LEVELS] = {}, oy[SP_MAX_LEVELS] = {};
        int64_t nx[SP_MAX_LEVELS] = {}, ny[SP_MAX_LEVELS] = {};   // the built clipmap's windows
        uint32_t localId[kLocalSlots] = {};
        int localFace[kLocalSlots] = {};
        bool sun = false;
        bool building = false;
        uint64_t shown = 0;      // shown_ then
    };
    struct Dynamic {
        bromath::AABB3 box;
        bool sways;
    };
    struct Rect {
        int map, mip, x, y, w, h;
        bool dynamic;
    };

    int localFirst(int slot) const { return config_.levels * SP_LEVEL_ENTRIES + slot * SP_LOCAL_ENTRIES; }
    /// The first entry of the shown clipmap (0) or the one built (past the
    /// local maps': the page table the lit shaders read ends before it).
    int clipFirst(bool next) const {
        return next ? config_.levels * SP_LEVEL_ENTRIES + kLocalSlots * SP_LOCAL_ENTRIES : 0;
    }
    bool isNext(int index) const { return index >= clipFirst(true); }
    const Sun& sunOf(bool next) const { return next ? next_ : sun_; }
    int mapOfEntry(int index, int& mip) const;
    float levelTexel(int level) const;
    static void sunPlane(const Sun& sun, const double p[3], double& a, double& b, double& c);
    void moveWindows(Sun& sun, int first);
    /// Begin `sun`'s light space along `d` about the eye, its plane
    /// coordinates there kept from `from`'s, its penumbra the sun's of
    /// `angularRadius` along `d` (both fixed for the clipmap's life, so its
    /// cached pages and its overlays part the plants alike).
    void beginBasis(Sun& sun, const double d[3], float angularRadius, const Sun* from) const;
    /// Fit `sun`'s depth range to the casters' `box` (and its casters'
    /// plane rect); true when a range it had must grow (grown only with
    /// `grow`).
    bool fitRange(Sun& sun, const bromath::AABB3& box, bool grow) const;
    /// `sun`'s levels' maps, appended to maps_.
    void addLevelMaps(const Sun& sun, bool next);
    /// The built clipmap becomes the shown one, the shown one's pages freed.
    void show();
    void release(int index);
    void releaseMap(int first, int count);
    /// Calls f(entry) for each entry of each map whose page (padded by the
    /// border) `box` touches, the clipmap levels minLevel to maxLevel (the
    /// built clipmap's too with `next`).
    template <class F> void forPages(const bromath::AABB3& box, int minLevel, int maxLevel, bool next, F&& f);
    /// `sun`'s plane rect (a0, a1, b0, b1) of `box`.
    static void planeRect(const Sun& sun, const bromath::AABB3& box, double rect[4]);
    /// Calls f(entry) for each page of clipmap level map `m` the plane rect
    /// (padded by the border) touches.
    template <class F> void forLevelRect(const Map& m, const double rect[4], F&& f);
    /// Each plants page's load (load_): the elements each addLoad casts at
    /// its level, spread over the pages there its plane rect touches.
    void spreadLoads();
    /// Flag the pages the frame's moving bounds touch that want an overlay:
    /// needed ones, and the sway level's over a needed finer page.
    void markDynamic();
    /// Flag the frame's base pages (setSeen).
    void markBase();
    bool neededNow(const Entry& e) const;
    int allocate();
    void viewMatrix(const Map& m, const Rect& r, ShadowViewPlan& v) const;

    ShadowPagesConfig config_;
    std::vector<Entry> entries_;
    std::vector<int32_t> owner_;      // each physical page's entry (-1 free)
    std::vector<int32_t> free_;
    Sun sun_;                         // the clipmap shown
    Sun next_;                        // the one built (building_)
    bool building_ = false;
    bool ready_ = false;              // the built one's needed pages are all drawn
    uint64_t begun_ = 0;              // the serial it was begun in
    uint64_t shown_ = 0;              // the clipmaps shown so far
    LocalSlot locals_[kLocalSlots];
    std::vector<Map> maps_;           // this frame's: the sun's levels, then local faces
    std::vector<Dynamic> dynamic_;
    std::vector<bromath::AABB3> invalid_;   // invalidate()s met before endLights(), applied there
    struct Load {
        bromath::AABB3 box;
        float elements[SP_MAX_LEVELS];
    };
    std::vector<Load> loads_;
    std::vector<float> load_;         // each entry's elements this frame (plan())
    /// A plants page's units: its texels and the elements cast into it.
    double plantUnits(int entry) const {
        return static_cast<double>(SP_PAGE) * SP_PAGE + static_cast<double>(load_[static_cast<size_t>(entry)]);
    }
    bool lightsSet_ = false;
    bromath::AABB3 seen_ = bromath::aempty3();
    History history_[8];
    uint64_t serial_ = 0;
    uint64_t newestRequest_ = 0;      // the newest plan serial requests came back for
    bromath::Vec3 eye_{0.0f, 0.0f, 0.0f};
    std::vector<uint64_t> granted_;
    std::vector<ShadowPageWant> wants_;
    ShadowPagesStats stats_;
};

/// The rects covering `grid`'s set cells (w x h, row-major), none wider or
/// taller than `maxSide`: each the row run from the first set cell left
/// grown down while every cell under it is set. With `block` > 1, the cells
/// are blocks of block x block (any set cell sets its block) and the rects
/// are in cells again, clipped to the grid.
void shadowCoverRects(const std::vector<uint8_t>& grid, int w, int h, int maxSide, int block,
                      std::vector<int>& out /* x, y, w, h each */);

}  // namespace bro::scene
