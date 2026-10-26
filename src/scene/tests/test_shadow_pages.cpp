// ShadowPages (scene/world/shadow_pages.h), the cached shadow pages' paging
// policy, and ShadowCasters (scene/world/shadow_casters.h): page addressing
// and the clipmap's snapping, the requests a marking pass reads back (late,
// across a window's move), the jobs it offers and what a grant draws, the
// levels a want's error and the guard's requirement come from, the pool's
// least-recently-needed reuse, invalidation by bounds and by a light's move,
// the sun's next clipmap (begun by a turn or an outgrown depth range, built
// under the shown one's wants, shown when drawn or after buildFrames,
// requests read late across the show), the overlays of what moves (from the sway level up,
// the finer levels reading the sway level's), the rect covers, and the
// casters' still and moving states, and the loads that price a plants page.

#include "scene/world/shadow_casters.h"
#include "scene/world/shadow_pages.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bro::scene;
using bromath::Vec3;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

ShadowSun downSun(Vec3 dir = {0.0f, -1.0f, 0.0f}) {
    ShadowSun s;
    s.lightId = 7;
    s.direction = dir;
    s.casters = {{-50.0f, -5.0f, -50.0f}, {50.0f, 30.0f, 50.0f}};
    return s;
}

/// The requests that name `entries` (a bit an entry).
std::vector<uint32_t> bitsOf(const ShadowPages& p, const std::vector<int>& entries) {
    std::vector<uint32_t> bits(static_cast<size_t>(p.entryCount() + 31) / 32, 0u);
    for (int e : entries) bits[static_cast<size_t>(e) / 32] |= 1u << (e % 32);
    return bits;
}

/// The level-`level` entry under `p` (the sun's).
int entryAt(const ShadowPages& pages, const Vec3& p, int level) {
    double a, b, c;
    pages.sunCoordinates(p, a, b, c);
    const double size = pages.pageSize(level);
    return pages.clipmapEntry(level, ShadowPages::pageOf(a, size), ShadowPages::pageOf(b, size));
}

/// One frame under the sun at `eye`: the requests (for this frame's own
/// plan) name `need`; every want is granted for the next frame.
void frame(ShadowPages& p, ShadowPlan& plan, const Vec3& eye, const std::vector<int>& need,
           const ShadowSun& sun = downSun(), bool grantAll = true) {
    p.beginFrame(eye);
    p.setSun(sun);
    p.endLights();
    if (!need.empty()) p.request(p.serial(), bitsOf(p, need));
    p.plan(plan);
    std::vector<uint64_t> keys;
    if (grantAll)
        for (const ShadowPageWant& w : p.wants()) keys.push_back(w.key);
    p.grant(keys);
}

bool hasWant(const ShadowPages& p, int entry, ShadowPageWant* out = nullptr) {
    for (const ShadowPageWant& w : p.wants())
        if (w.key == static_cast<uint64_t>(entry)) {
            if (out) *out = w;
            return true;
        }
    return false;
}

void testAddressingAndSnapping() {
    ShadowPages p;
    ShadowPlan plan;
    // A new sun's plane coordinates start at the eye: (0, 0) is a page
    // corner. Straight down, a runs along -x and b along -z, so the eye half
    // a page back on both is a page's centre.
    frame(p, plan, Vec3{0.0f, 1.0f, 0.0f}, {});
    const Vec3 eye{-0.0625f, 1.0f, -0.0625f};
    double a, b, c;
    p.sunCoordinates(eye, a, b, c);
    check(std::fabs(a - 0.0625) < 1e-6 && std::fabs(b - 0.0625) < 1e-6 && std::fabs(c) < 1e-6,
          "a new sun's plane coordinates are the eye's offsets across the light");
    frame(p, plan, eye, {});
    check(p.hasSun() && plan.mapCount == SP_MAX_LEVELS, "the sun's levels are the plan's maps");
    check(std::fabs(p.pageSize(0) - 0.125) < 1e-12 && std::fabs(p.pageSize(3) - 1.0) < 1e-12,
          "a page is 128 texels: 0.125 m at level 0, twice that a level up");
    int64_t ox, oy;
    p.window(0, ox, oy);
    check(ox == ShadowPages::pageOf(a, 0.125) - SP_WINDOW / 2 && oy == ShadowPages::pageOf(b, 0.125) - SP_WINDOW / 2,
          "a level's window is centred on the eye's page");
    // Moving inside the eye's page moves no window; a page across moves
    // each finer window by one page and keeps the pages both windows hold.
    const int e = entryAt(p, eye, 0);
    frame(p, plan, eye, {e});
    frame(p, plan, eye, {e});   // drawn (granted last frame)
    check(p.entry(e).drawn && p.entry(e).physical >= 0, "a granted page is drawn the frame after");
    const int phys = p.entry(e).physical;
    frame(p, plan, eye + Vec3{0.05f, 0.0f, 0.0f}, {});
    int64_t ox2, oy2;
    p.window(0, ox2, oy2);
    check(ox2 == ox && oy2 == oy, "a move inside a page leaves the window");
    const Vec3 moved = eye + Vec3{0.125f, 0.0f, 0.0f};
    frame(p, plan, moved, {});
    p.window(0, ox2, oy2);
    check(std::llabs(ox2 - ox) + std::llabs(oy2 - oy) == 1, "a page's move shifts the window one page");
    const int e2 = entryAt(p, eye, 0);
    check(e2 == e && p.entry(e2).drawn && p.entry(e2).physical == phys,
          "a page still in the window keeps its slot, its depth and its physical page");
    // A level's map takes the eye (relative: the origin) to its page's
    // (u, v), with Vulkan's v down (toVulkanClip's flip).
    const ShadowMapPlan& m = plan.maps[0];
    const float u = 0.5f * (m.relViewProj.at(0, 3)) + 0.5f;
    const float v = 0.5f - 0.5f * (m.relViewProj.at(1, 3));
    const int px = static_cast<int>(std::floor(u * SP_WINDOW)), py = static_cast<int>(std::floor(v * SP_WINDOW));
    const ShadowPages::EntryView view = p.entry(entryAt(p, moved, 0));
    check(view.x == ox2 + px && view.y == oy2 + py, "the map's (u, v) at the eye is the eye's page");
    check(std::fabs(u * SP_WINDOW - px - 0.5f) < 1e-3f && std::fabs(v * SP_WINDOW - py - 0.5f) < 1e-3f,
          "the eye at its page's centre is at the centre of the map's page");
    check(m.originX == static_cast<int>(((ox2 % SP_WINDOW) + SP_WINDOW) % SP_WINDOW), "the window's origin mod the window");
}

void testRequestsWantsAndGrants() {
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{3.3f, 1.0f, -2.1f};
    frame(p, plan, eye, {}, downSun(), false);
    check(p.wants().empty() && plan.viewCount == 0, "nothing requested, nothing wanted or drawn");
    const int fine = entryAt(p, eye, 2), guard = entryAt(p, eye, 2 + SP_GUARD_LEVELS);
    frame(p, plan, eye, {fine, guard}, downSun(), false);
    ShadowPageWant wf, wg;
    check(hasWant(p, fine, &wf) && hasWant(p, guard, &wg), "the requested pages are wanted");
    check(wg.required && !wf.required, "the top of a chain is required, a page under a needed one is not");
    check(wf.error == 64.0f, "with nothing drawn above it a page is as wrong as can be");
    check(wg.size > wf.size, "a coarser page is the larger");
    check(wg.plants == (2 + SP_GUARD_LEVELS > p.swayLevel()) && !wf.plants,
          "a page the plants draw into (past the sway level) is the dearer kind");
    // Grant the guard alone: drawn next frame and sampled in it.
    p.grant(std::vector<uint64_t>{static_cast<uint64_t>(guard)});
    p.beginFrame(eye);
    p.setSun(downSun());
    p.endLights();
    p.request(p.serial(), bitsOf(p, {fine, guard}));
    p.plan(plan);
    check(plan.viewCount == 1 && plan.copyOut.size() == 1 && plan.staticPages == 1, "a grant is one view and one copy");
    check(plan.views[0].w == 1 && plan.views[0].h == 1 && !plan.views[0].dynamic, "a page's view is the page");
    check((plan.table[2 * static_cast<size_t>(guard)] & SP_PHYSICAL_MASK) ==
                  static_cast<uint32_t>(p.entry(guard).physical) &&
              (plan.table[2 * static_cast<size_t>(guard)] & SP_RESIDENT) != 0 &&
              plan.copyOut[0].physical == p.entry(guard).physical,
          "a page drawn this frame is copied into its physical page and sampled there this frame");
    check(hasWant(p, fine, &wf) && wf.error == 8.0f, "the page under a drawn page 3 levels up is 8 pixels wrong");
    check(!hasWant(p, guard), "a drawn page is not wanted");
    frame(p, plan, eye, {fine, guard}, downSun(), false);
    check((plan.table[2 * static_cast<size_t>(guard)] & SP_RESIDENT) != 0 &&
              (plan.table[2 * static_cast<size_t>(guard)] & SP_PHYSICAL_MASK) ==
                  static_cast<uint32_t>(p.entry(guard).physical) &&
              plan.copyOut.empty(),
          "the frame after, the table still finds it, with nothing redrawn");
    // A request read late: the plan it names is older, the page still in
    // the window, so it counts; one older than the last two does not.
    const uint64_t old = p.serial();
    frame(p, plan, eye, {}, downSun(), false);
    frame(p, plan, eye, {}, downSun(), false);
    check(p.entry(fine).needed, "a page stays needed until requests stop naming it");
    p.beginFrame(eye);
    p.setSun(downSun());
    p.endLights();
    p.request(old, bitsOf(p, {fine}));
    p.request(p.serial(), bitsOf(p, {}));
    p.plan(plan);
    check(!p.entry(fine).needed, "a page the newest requests do not name is not needed");
}

void testStillCrowns() {
    // A low sun's penumbra hides a tall crown's sway: the cached pages at
    // the sway level and finer take those crowns, the overlay the rest,
    // parted by the clipmap's own penumbra; past the sway level, all.
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{3.3f, 1.0f, -2.1f};
    ShadowSun sun = downSun(bromath::vnorm(Vec3{std::sqrt(3.0f), -1.0f, 0.0f}));   // 30 degrees up
    sun.angularRadius = 0.005f;
    const int s = p.swayLevel();
    const int fine = entryAt(p, eye, s - 1), coarse = entryAt(p, eye, s + 1);
    frame(p, plan, eye, {fine, coarse}, sun);
    frame(p, plan, eye, {fine, coarse}, sun);
    int still = 0, all = 0;
    for (int v = 0; v < plan.viewCount; ++v) {
        const ShadowViewPlan& view = plan.views[v];
        if (view.dynamic) continue;
        const int level = plan.maps[view.map].level;
        if (level <= s) still += view.plants == ShadowPlants::Still;
        else all += view.plants == ShadowPlants::All;
        check(std::fabs(view.penumbra - 0.02f) < 1e-5f, "a view's penumbra is the disc's over the sun's height");
        check(!shadowCastsInto(view, ShadowMotion::Swaying) || level > s,
              "only the plants part their crowns: what else sways stays off the near pages");
    }
    check(still > 0 && all > 0, "the near pages take the still crowns, the coarse ones every plant");
    uint32_t copies = 0;
    for (const ShadowPageCopy& c : plan.copyOut) copies += c.plants;
    check(copies == plan.plantPages && plan.plantPages > 0 && plan.plantPages < plan.staticPages,
          "only the pages every plant draws into are the plants' kind");
}

void testLoads() {
    // A plants page is priced by its texels and the elements cast into it;
    // a still page is a page.
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{0.0625f, 1.0f, 0.0625f};
    const int s = p.swayLevel(), level = s + 1;
    const Vec3 far = eye + Vec3{3.0f * static_cast<float>(p.pageSize(level)), 0.0f, 0.0f};
    float elements[SP_MAX_LEVELS] = {};
    elements[s] = 7000.0f;   // (no level the plants' pages are not)
    elements[level] = 1000.0f;
    p.beginFrame(eye);
    p.setSun(downSun());
    p.endLights();
    const int loaded = entryAt(p, eye, level), empty = entryAt(p, far, level), still = entryAt(p, eye, s);
    p.addLoad({eye - Vec3{0.5f, 0.5f, 0.5f}, eye + Vec3{0.5f, 0.5f, 0.5f}}, elements);
    p.request(p.serial(), bitsOf(p, {loaded, empty, still}));
    p.plan(plan);
    const double page = static_cast<double>(SP_PAGE) * SP_PAGE;
    ShadowPageWant a, b, c;
    check(hasWant(p, loaded, &a) && hasWant(p, empty, &b) && hasWant(p, still, &c), "each page is wanted");
    check(a.plants && a.units > page && a.units <= page + 1000.0, "a page a load falls on costs its share");
    check(loaded != empty && b.plants && b.units == page, "one none does its texels");
    check(!c.plants && c.units == 1.0, "a still page is one page");
}

void testWindowMoveAndLateRequests() {
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{0.0625f, 1.0f, 0.0625f};
    frame(p, plan, eye, {}, downSun(), false);
    const uint64_t serial = p.serial();
    const int e = entryAt(p, eye, 0);
    // Requests for the frame before the eye moved across many pages: the
    // page they name has left the window (its slot holds another page).
    frame(p, plan, eye + Vec3{0.125f * 40.0f, 0.0f, 0.0f}, {}, downSun(), false);
    p.request(serial, bitsOf(p, {e}));
    check(!p.entry(e).needed, "a request for a page gone from the window is dropped");
    frame(p, plan, eye, {}, downSun(), false);
}

void testLruReuse() {
    ShadowPagesConfig c;
    c.poolColumns = 3;
    c.poolRows = 1;       // three pages
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.5f, 1.0f, 0.5f};
    frame(p, plan, eye, {});
    std::vector<int> a;
    for (int i = 0; i < 3; ++i) a.push_back(entryAt(p, eye + Vec3{-0.13f * i, 0.0f, 0.0f}, 0));
    frame(p, plan, eye, a);
    frame(p, plan, eye, a);
    bool all = true;
    for (int e : a) all = all && p.entry(e).drawn;
    check(all, "three pages fill the pool");
    // Two new pages wanted while one of the three stays needed: the two
    // least recently needed go.
    const int b0 = entryAt(p, eye + Vec3{0.0f, 0.0f, -0.13f}, 0), b1 = entryAt(p, eye + Vec3{0.0f, 0.0f, -0.26f}, 0);
    frame(p, plan, eye, {a[0], b0, b1});
    frame(p, plan, eye, {a[0], b0, b1});
    check(p.entry(b0).drawn && p.entry(b1).drawn && p.entry(a[0]).drawn, "the needed pages hold the pool");
    check(!p.entry(a[1]).drawn && !p.entry(a[2]).drawn, "the pages no longer needed were reused");
    check(p.stats().evicted == 2, "two evictions, counted in the frame that drew over them");
    // Needed beyond the pool: what does not fit waits.
    frame(p, plan, eye, {a[0], a[1], b0, b1});
    frame(p, plan, eye, {a[0], a[1], b0, b1});
    int drawn = 0;
    for (int e : {a[0], a[1], b0, b1}) drawn += p.entry(e).drawn;
    check(drawn == 3, "a pool of three holds three needed pages, the fourth waits");
}

void testPoolShort() {
    // More needed than the pool holds: the levels are taken coarser for a
    // pixel (a planet's view across its land wanted 6,626 pages of 3,600,
    // and the grants that found no page went undrawn every frame), held a
    // few frames, and finer again when the view needs few.
    ShadowPagesConfig c;
    c.poolColumns = 10;
    c.poolRows = 1;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.5f, 1.0f, 0.5f};
    frame(p, plan, eye, {});
    check(p.footprintScale() == 1.0f, "a pixel's footprint as it is while the pool holds what is needed");
    std::vector<int> many;
    for (int i = 0; i < 10; ++i) many.push_back(entryAt(p, eye + Vec3{-0.13f * i, 0.0f, 0.0f}, 0));
    frame(p, plan, eye, many);
    check(p.footprintScale() == 2.0f, "ten needed of ten: a level coarser");
    for (int f = 0; f < 4; ++f) frame(p, plan, eye, many);
    check(p.footprintScale() == 2.0f, "held while its requests come back");
    for (int f = 0; f < 8; ++f) frame(p, plan, eye, many);
    check(p.footprintScale() == 4.0f, "still short: another level");
    for (int f = 0; f < 10; ++f) frame(p, plan, eye, {many[0]});
    check(p.footprintScale() == 2.0f, "one needed: a level finer again");
    for (int f = 0; f < 10; ++f) frame(p, plan, eye, {many[0]});
    check(p.footprintScale() == 1.0f, "and back to a pixel");

    // Needed past the pool: no page is required (each would be granted
    // every frame and find no page); within it the tops of chains are.
    ShadowPages q;
    q.configure(c);
    frame(q, plan, eye, {});
    std::vector<int> over;
    for (int i = 0; i < 12; ++i) over.push_back(entryAt(q, eye + Vec3{-0.13f * i, 0.0f, 0.0f}, 0));
    frame(q, plan, eye, over, downSun(), false);
    int required = 0;
    for (const ShadowPageWant& w : q.wants()) required += w.required;
    check(required == 0, "twelve needed of ten: none required (" + std::to_string(required) + ")");
    for (int f = 0; f < 4; ++f) frame(q, plan, eye, {over[0], over[1]}, downSun(), false);
    required = 0;
    for (const ShadowPageWant& w : q.wants()) required += w.required;
    check(required == 2, "two needed of ten: both required (" + std::to_string(required) + ")");
}

void testInvalidation() {
    // With no stale page redrawn the frame it goes stale (staleNow 0), what
    // the world service is left: testStaleNow has the default.
    ShadowPagesConfig c;
    c.staleNow = 0;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int near = entryAt(p, eye, 4), far = entryAt(p, eye + Vec3{20.0f, 0.0f, 0.0f}, 4);
    frame(p, plan, eye, {near, far});
    frame(p, plan, eye, {near, far});
    check(p.entry(near).drawn && p.entry(far).drawn && !p.entry(near).dirty, "both drawn");
    // A caster's change under the eye: its pages are stale, the far one is not.
    p.beginFrame(eye);
    p.setSun(downSun());
    p.endLights();
    p.invalidate({{-0.5f, 0.0f, -0.5f}, {0.5f, 2.0f, 0.5f}});
    p.request(p.serial(), bitsOf(p, {near, far}));
    p.plan(plan);
    check(p.entry(near).dirty && !p.entry(far).dirty, "a caster's bounds invalidate the pages they touch");
    ShadowPageWant w;
    check(hasWant(p, near, &w) && w.error == 1.0f && !w.required, "a stale page is a pixel wrong, and draws meanwhile");
    check((plan.table[2 * static_cast<size_t>(near)] & SP_RESIDENT) != 0, "a stale page keeps being sampled");
    std::vector<uint64_t> keys;
    for (const ShadowPageWant& x : p.wants()) keys.push_back(x.key);
    p.grant(keys);
    frame(p, plan, eye, {near, far});
    check(!p.entry(near).dirty && plan.staticPages == 1, "redrawn in place, it is clean");
    // The sun turning under the threshold keeps every page and begins
    // nothing.
    frame(p, plan, eye, {near, far}, downSun(bromath::vnorm(Vec3{std::sin(0.001f), -1.0f, 0.0f})));
    check(!p.entry(near).dirty && !p.entry(far).dirty && !p.building(), "a turn under the threshold keeps the pages");
}

/// The level-`level` entry under `p` in the clipmap being built.
int nextEntryAt(const ShadowPages& pages, const Vec3& p, int level) {
    double a, b, c;
    pages.sunCoordinates(p, a, b, c, true);
    const double size = pages.pageSize(level);
    return pages.clipmapEntry(level, ShadowPages::pageOf(a, size), ShadowPages::pageOf(b, size), true);
}

void testSunTurnBuildsTheNext() {
    ShadowPagesConfig c;
    c.staleNow = 0;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int near = entryAt(p, eye, 4);
    frame(p, plan, eye, {near});
    frame(p, plan, eye, {near});
    check(p.entry(near).drawn, "drawn");
    const int phys = p.entry(near).physical;

    // Past the threshold the shown pages stay clean and sampled while the
    // next clipmap, in light space of the sun as it is now, is built.
    // (Its own frame's requests would name the next pages too, the mark pass
    // marking both; none are read back yet.)
    const ShadowSun turned = downSun(bromath::vnorm(Vec3{std::sin(0.01f), -1.0f, 0.0f}));
    frame(p, plan, eye, {}, turned, false);
    check(p.building() && !p.entry(near).dirty && (plan.table[2 * static_cast<size_t>(near)] & SP_RESIDENT) != 0,
          "a turn past the threshold begins the next clipmap and keeps the shown one");
    check(plan.nextCount == SP_MAX_LEVELS && plan.mapCount == SP_MAX_LEVELS, "its levels are planned apart");
    check(plan.table.size() / 2 < static_cast<size_t>(plan.entryCount) && plan.entryCount == p.entryCount(),
          "the page table ends before its entries; the requests run on past");
    const int next = nextEntryAt(p, eye, 4);
    check(next >= static_cast<int>(plan.table.size() / 2), "its entries are past the table");
    double a0, b0, c0, a1, b1, c1;
    p.sunCoordinates(eye, a0, b0, c0);
    p.sunCoordinates(eye, a1, b1, c1, true);
    check(std::fabs(a0 - a1) < 1e-9 && std::fabs(b0 - b1) < 1e-9, "it keeps the eye's plane coordinates");
    // Its pages are wanted under a pixel and never required.
    frame(p, plan, eye, {near, next}, turned, false);
    ShadowPageWant w;
    check(hasWant(p, next, &w) && w.error < 1.0f && !w.required && p.stats().building == 1,
          "its pages wait under every shown page");
    // A further turn while it is built waits for it.
    const ShadowSun further = downSun(bromath::vnorm(Vec3{std::sin(0.02f), -1.0f, 0.0f}));
    frame(p, plan, eye, {near, next}, further, false);
    check(p.building() && nextEntryAt(p, eye, 4) == next, "a turn while one is built waits");
    // Granted and drawn, the frame after it is shown: the shown entries are
    // its, the old pages freed.
    p.grant(std::vector<uint64_t>{static_cast<uint64_t>(next)});
    frame(p, plan, eye, {near, next}, further, false);
    check(p.entry(next).drawn && p.stats().building == 0 && plan.staticPages == 1, "drawn");
    const int nextPhys = p.entry(next).physical;
    frame(p, plan, eye, {near}, further, false);
    const int shown = entryAt(p, eye, 4);
    check(shown == near && p.entry(shown).drawn && p.entry(shown).physical == nextPhys,
          "once its needed pages are drawn it is shown, its pages kept");
    check(p.stats().resident >= 1 &&
              (plan.table[2 * static_cast<size_t>(shown)] & SP_PHYSICAL_MASK) == static_cast<uint32_t>(nextPhys),
          "and sampled");
    // The sun has turned on since (further): the next one begins at once.
    check(p.building(), "a turn past the one shown begins another");
    (void)phys;
}

void testBuildAcrossShowAndRange() {
    ShadowPagesConfig c;
    c.staleNow = 0;
    c.buildFrames = 3;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int near = entryAt(p, eye, 4);
    frame(p, plan, eye, {near});
    frame(p, plan, eye, {near});
    // A caster beyond the depth range: the shown pages stay clean, the next
    // clipmap (the same sun, a range that holds it) is built.
    ShadowSun tall = downSun();
    tall.casters.max.y = 900.0f;
    frame(p, plan, eye, {near}, tall, false);
    check(p.building() && !p.entry(near).dirty, "an outgrown depth range builds the next clipmap");
    const int next = nextEntryAt(p, eye, 4);
    const uint64_t serial = p.serial();
    // Nothing granted: after buildFrames it is shown anyway (its pages then
    // wanted as the shown one's), and a request read late for its page
    // names the shown one's.
    frame(p, plan, eye, {}, tall, false);
    frame(p, plan, eye, {}, tall, false);
    frame(p, plan, eye, {}, tall, false);
    check(!p.building() && !p.entry(near).drawn, "after buildFrames it is shown, drawn or not");
    p.beginFrame(eye);
    p.setSun(tall);
    p.endLights();
    p.request(serial, bitsOf(p, {next}));
    p.plan(plan);
    check(p.entry(near).needed && hasWant(p, near), "a late request for the built page names the shown one");
}

void testOverlays() {
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int s = p.swayLevel();
    check(s == 3, "the sway level is the one whose texel is about 8 mm");
    const int l0 = entryAt(p, eye, 0), ls = entryAt(p, eye, s), l6 = entryAt(p, eye, 6);
    frame(p, plan, eye, {l0, l6});
    frame(p, plan, eye, {l0, l6});
    const auto moving = [&](bool sways, std::initializer_list<int> needed) {
        p.beginFrame(eye);
        p.setSun(downSun());
        p.endLights();
        p.addDynamic({{-0.2f, 0.0f, -0.2f}, {0.2f, 3.0f, 0.2f}}, sways);
        if (needed.size()) p.request(p.serial(), bitsOf(p, needed));
        p.plan(plan);
    };
    // Something swaying at the eye: its overlay is the sway level's page
    // over the needed level 0 page (not itself needed), and none other.
    moving(true, {l0, l6});
    const size_t i0 = 2 * static_cast<size_t>(l0), is = 2 * static_cast<size_t>(ls), i6 = 2 * static_cast<size_t>(l6);
    check(plan.viewCount == 1 && plan.views[0].dynamic && plan.views[0].w == 1 && plan.views[0].h == 1,
          "an overlay is a dynamic view of the sway level's page");
    check(plan.copyOut.empty(), "an overlay is not copied: it is sampled where it is drawn");
    check((plan.table[i0] & SP_PHYSICAL_MASK) == static_cast<uint32_t>(p.entry(l0).physical) && plan.table[i0 + 1] == 0u,
          "the level 0 page samples its cached depth and has no overlay of its own");
    const uint32_t os = plan.table[is + 1];
    check((os & SP_RESIDENT) && (os & SP_OVERLAY_MASK) == static_cast<uint32_t>(plan.views[0].scratchX + SP_BORDER) &&
              ((os >> SP_OVERLAY_BITS) & SP_OVERLAY_MASK) == static_cast<uint32_t>(plan.views[0].scratchY + SP_BORDER),
          "the sway level's overlay word names its page's first texel in the scratch target");
    check(plan.table[i6 + 1] == 0u, "what sways has no overlay past the sway level");
    check(plan.views[0].plants == ShadowPlants::Swaying && plan.views[0].grass,
          "the sway level's overlay takes the swaying plants and the grass");
    check(shadowCastsInto(plan.views[0], ShadowMotion::Swaying), "and whatever else sways");
    // What moves (not sways) also overlays the coarser needed pages.
    moving(false, {l0, l6});
    check((plan.table[is + 1] & SP_RESIDENT) && (plan.table[i6 + 1] & SP_RESIDENT) && plan.table[i0 + 1] == 0u,
          "what moves overlays the sway level and the coarser levels");
    for (int v = 0; v < plan.viewCount; ++v)
        if (plan.views[v].map == 6)
            check(plan.views[v].plants == ShadowPlants::None && !plan.views[v].grass, "a coarser overlay takes no sway");
    // The requests come back a frame or two late: a page needed one plan
    // ago is still needed.
    moving(true, {});
    check(plan.table[is + 1] != 0u, "a page needed a frame ago keeps its overlay");
}

void testCoverRects() {
    std::vector<uint8_t> grid(8 * 8, 0);
    for (int y = 1; y < 4; ++y)
        for (int x = 2; x < 6; ++x) grid[static_cast<size_t>(y) * 8 + x] = 1;
    grid[7 * 8 + 7] = 1;
    std::vector<int> r;
    shadowCoverRects(grid, 8, 8, 31, 1, r);
    check(r.size() == 8 && r[0] == 2 && r[1] == 1 && r[2] == 4 && r[3] == 3 && r[4] == 7 && r[5] == 7,
          "a block and a lone page: two exact rects");
    shadowCoverRects(grid, 8, 8, 2, 1, r);
    int area = 0;
    bool small = true;
    for (size_t k = 0; k < r.size(); k += 4) {
        area += r[k + 2] * r[k + 3];
        small = small && r[k + 2] <= 2 && r[k + 3] <= 2;
    }
    check(area == 13 && small, "a side limit cuts the cover, still exact");
    shadowCoverRects(grid, 8, 8, 31, 4, r);
    check(r.size() == 8 && r[0] == 0 && r[1] == 0 && r[2] == 8 && r[3] == 4 && r[4] == 4 && r[5] == 4 &&
              r[6] == 4 && r[7] == 4,
          "in 4 x 4 blocks the cover is coarser and holds every page");
}

void testLocalMaps() {
    ShadowPages p;
    ShadowPlan plan;
    ShadowLocal spot;
    spot.lightId = 11;
    spot.lightIndex = 1;
    spot.faces = 1;
    spot.viewProj[0] = bromath::midentity();
    spot.relViewProj[0] = bromath::midentity();
    spot.texelPerMetre = 0.002f;
    p.beginFrame({0.0f, 0.0f, 0.0f});
    p.setSun(downSun());
    check(p.addLocal(spot), "a spot takes a map");
    p.endLights();
    p.plan(plan);
    check(plan.mapCount == SP_MAX_LEVELS + 1 && plan.lightMap[1] == SP_MAX_LEVELS && plan.lightMapCount[1] == 1,
          "after the sun's levels");
    const int first = plan.maps[SP_MAX_LEVELS].firstEntry;
    const int top = first + SP_LOCAL_MIP_FIRST(3), fine = first + 9;   // mip 0, page (1, 1)
    for (int f = 0; f < 3; ++f) {
        p.beginFrame({0.0f, 0.0f, 0.0f});
        p.setSun(downSun());
        p.addLocal(spot);
        p.endLights();
        p.request(p.serial(), bitsOf(p, {top, fine}));
        p.plan(plan);
        if (f == 0) check(p.entry(top).drawn && !hasWant(p, top), "a local map's one-page mip is a base page: drawn now");
        std::vector<uint64_t> keys;
        for (const ShadowPageWant& x : p.wants()) keys.push_back(x.key);
        p.grant(keys);
    }
    check(p.entry(top).drawn && p.entry(fine).drawn, "its pages are drawn");
    // The light moves: its pages are stale.
    spot.viewProj[0].at(0, 3) = 0.5f;
    p.beginFrame({0.0f, 0.0f, 0.0f});
    p.setSun(downSun());
    p.addLocal(spot);
    p.endLights();
    p.request(p.serial(), bitsOf(p, {top, fine}));
    p.plan(plan);
    check(!p.entry(fine).dirty && !p.entry(top).dirty && plan.staticPages == 2,
          "a light that moves redraws its pages, the frame it moves");
    // A frame without it gives its pages back.
    p.beginFrame({0.0f, 0.0f, 0.0f});
    p.setSun(downSun());
    p.endLights();
    p.plan(plan);
    check(!p.entry(fine).drawn && p.entry(fine).physical < 0, "a light gone gives its pages back");
}

void testStaleNow() {
    // By default a stale needed page is redrawn the frame it goes stale; a
    // stale page no one needs is not sampled.
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int near = entryAt(p, eye, 4), other = entryAt(p, eye + Vec3{0.0f, 0.0f, 0.3f}, 3);
    frame(p, plan, eye, {near, other});
    frame(p, plan, eye, {near, other});
    check(p.entry(near).drawn && p.entry(other).drawn, "both drawn");
    // Needed no more: `other` stays resident while clean...
    frame(p, plan, eye, {near});
    frame(p, plan, eye, {near});
    frame(p, plan, eye, {near});
    check(p.entry(other).drawn && (plan.table[2 * static_cast<size_t>(other)] & SP_RESIDENT) != 0,
          "a page no longer needed is still sampled while it holds");
    p.beginFrame(eye);
    p.setSun(downSun());
    p.invalidate({{-0.5f, 0.0f, -0.5f}, {0.5f, 2.0f, 0.5f}});   // (met before the lights: waits for them)
    p.endLights();
    p.request(p.serial(), bitsOf(p, {near}));
    p.plan(plan);
    check(!p.entry(near).dirty && plan.staticPages >= 1, "a stale needed page is redrawn the frame it goes stale");
    check(p.entry(other).dirty && (plan.table[2 * static_cast<size_t>(other)] & SP_RESIDENT) == 0,
          "a stale page no one needs is not sampled");
}

void testBase() {
    // What is seen: the base pages cover it, drawn the first frame, with
    // nothing requested yet.
    ShadowPages p;
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    ShadowSun sun = downSun();
    sun.casters = {{-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}};
    p.beginFrame(eye);
    p.setSun(sun);
    p.endLights();
    p.setSeen({{-20.0f, 0.0f, -20.0f}, {20.0f, 5.0f, 20.0f}});
    p.plan(plan);
    // The casters' 2 m across the light in 4 pages: level 2 (0.5 m pages).
    const int e = entryAt(p, eye, 2);
    check(plan.staticPages > 0 && p.entry(e).drawn && (plan.table[2 * static_cast<size_t>(e)] & SP_RESIDENT) != 0,
          "the first frame draws the base pages and samples them");
    check(p.stats().needed == plan.staticPages && p.wants().empty(), "base pages are needed, and none waits");
    check(!p.entry(entryAt(p, eye + Vec3{5.0f, 0.0f, 0.0f}, 2)).drawn, "where no shadow can fall nothing is drawn");
}

void testBaseCapped() {
    // Where even the coarsest level spans what is seen in more than its
    // base's pages (the land to the horizon from high up): only that many
    // pages about the eye, never the whole window the pool cannot hold.
    ShadowPagesConfig c;
    c.levels = 3;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    ShadowSun sun = downSun();
    sun.casters = {{-1.0e5f, 0.0f, -1.0e5f}, {1.0e5f, 2.0f, 1.0e5f}};
    p.beginFrame(eye);
    p.setSun(sun);
    p.endLights();
    p.setSeen({{-1.0e5f, 0.0f, -1.0e5f}, {1.0e5f, 5.0f, 1.0e5f}});
    p.plan(plan);
    check(p.stats().needed > 0 && p.stats().needed <= 36, "a base wider than the coarsest level's is its base's pages");
    check(p.wants().empty() && p.entry(entryAt(p, eye, 2)).drawn, "drawn at once about the eye, none waiting");
}

void testCasters() {
    ShadowPagesConfig c;
    c.staleNow = 0;   // (the redraws left to the world service, so they show)
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    ShadowCasters casters;
    const Vec3 eye{0.0f, 1.0f, 0.0f};
    frame(p, plan, eye, {});
    const int e = entryAt(p, eye, 3);
    frame(p, plan, eye, {e});
    frame(p, plan, eye, {e});
    const bromath::AABB3 box{{-0.3f, 0.0f, -0.3f}, {0.3f, 1.0f, 0.3f}};
    const auto step = [&](uint64_t gen, bool always) {
        p.beginFrame(eye);
        p.setSun(downSun());
        p.endLights();
        casters.begin();
        const bool moving = casters.caster(5, gen, box, always, p);
        casters.end(p);
        p.request(p.serial(), bitsOf(p, {e}));
        p.plan(plan);
        std::vector<uint64_t> keys;
        for (const ShadowPageWant& x : p.wants()) keys.push_back(x.key);
        p.grant(keys);
        return moving;
    };
    check(!step(1, false) && p.entry(e).dirty, "a still caster that appears redraws the pages it touches");
    step(1, false);
    check(!p.entry(e).dirty, "and they are redrawn");
    check(step(2, false) && p.entry(e).dirty, "a change moves it: out of the cached depth");
    bool moving = true;
    int frames = 0;
    while (moving && frames < 20) {
        moving = step(2, false);
        ++frames;
    }
    check(frames == ShadowCasters::kSettleFrames && p.entry(e).dirty, "still for a while, it rests: back in the depth");
    check(step(2, true), "a caster told to move moves");
    // Gone: a still caster's pages are redrawn without it.
    step(2, false);
    for (int i = 0; i < ShadowCasters::kSettleFrames + 2; ++i) step(2, false);
    p.beginFrame(eye);
    p.setSun(downSun());
    p.endLights();
    casters.begin();
    casters.end(p);
    check(p.entry(e).dirty && !casters.moving(5), "a still caster gone redraws its pages");
}

void testRedrawPlantsOf() {
    // The plants drawn in one frame came up short (their plan out of room):
    // the pages that frame drew are missing again, and only those.
    ShadowPagesConfig c;
    c.staleNow = 0;
    ShadowPages p;
    p.configure(c);
    ShadowPlan plan;
    const Vec3 eye{0.0625f, 1.0f, 0.0625f};
    const int s = p.swayLevel();
    const int first = entryAt(p, eye, s + 1), second = entryAt(p, eye, s + 2);
    frame(p, plan, eye, {first});
    frame(p, plan, eye, {first});   // drawn (granted last frame)
    const uint64_t drewFirst = p.serial();
    frame(p, plan, eye, {first, second});
    frame(p, plan, eye, {first, second});
    const uint64_t drewSecond = p.serial();
    check(p.entry(first).drawn && p.entry(second).drawn && drewFirst != drewSecond, "each drawn in its own frame");
    p.redrawPlantsOf(drewSecond + 7);
    check(p.entry(first).drawn && p.entry(second).drawn, "a frame that drew no page leaves them all");
    p.redrawPlantsOf(drewFirst);
    check(!p.entry(first).drawn && p.entry(first).physical < 0 && p.entry(second).drawn,
          "a short frame's page is missing again, another frame's is kept");
    frame(p, plan, eye, {first, second});
    ShadowPageWant w;
    check(hasWant(p, first, &w) && w.error > 1.0f, "and waits as a missing page, not a stale one");
}

}  // namespace

int main() {
    testAddressingAndSnapping();
    testRequestsWantsAndGrants();
    testStillCrowns();
    testLoads();
    testWindowMoveAndLateRequests();
    testLruReuse();
    testPoolShort();
    testInvalidation();
    testSunTurnBuildsTheNext();
    testBuildAcrossShowAndRange();
    testStaleNow();
    testBase();
    testBaseCapped();
    testOverlays();
    testCoverRects();
    testLocalMaps();
    testCasters();
    testRedrawPlantsOf();
    std::printf("bro_shadow_pages_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
