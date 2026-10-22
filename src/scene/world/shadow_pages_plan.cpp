// ShadowPages' frame (shadow_pages.h): what it draws, its page table and the
// world service's jobs.

#include "scene/world/shadow_pages.h"

#include "scene/depth_policy.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

int64_t wrap(int64_t v) { return ((v % SP_WINDOW) + SP_WINDOW) % SP_WINDOW; }

// A page of the clipmap built is wrong by nothing on screen yet: its error
// is under a pixel, under every page shown.
constexpr float kNextError = 0.5f;

}  // namespace

void shadowCoverRects(const std::vector<uint8_t>& grid, int w, int h, int maxSide, int block, std::vector<int>& out) {
    out.clear();
    block = std::max(block, 1);
    const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
    std::vector<uint8_t> set(static_cast<size_t>(bw) * bh, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (grid[static_cast<size_t>(y) * w + x]) set[static_cast<size_t>(y / block) * bw + x / block] = 1;
    const int side = std::max(1, maxSide / block);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            if (!set[static_cast<size_t>(y) * bw + x]) continue;
            int rw = 1;
            while (x + rw < bw && rw < side && set[static_cast<size_t>(y) * bw + x + rw]) ++rw;
            int rh = 1;
            for (; y + rh < bh && rh < side; ++rh) {
                bool full = true;
                for (int i = 0; i < rw && full; ++i) full = set[static_cast<size_t>(y + rh) * bw + x + i] != 0;
                if (!full) break;
            }
            for (int j = 0; j < rh; ++j)
                for (int i = 0; i < rw; ++i) set[static_cast<size_t>(y + j) * bw + x + i] = 0;
            const int cx = x * block, cy = y * block;
            out.insert(out.end(), {cx, cy, std::min(rw * block, w - cx), std::min(rh * block, h - cy)});
        }
}

int ShadowPages::allocate() {
    if (!free_.empty()) {
        const int p = free_.back();
        free_.pop_back();
        return p;
    }
    // The least recently needed page that no one needs now.
    int best = -1;
    uint64_t oldest = ~0ull;
    for (size_t p = 0; p < owner_.size(); ++p) {
        const int o = owner_[p];
        if (o < 0) continue;
        const Entry& e = entries_[static_cast<size_t>(o)];
        if (neededNow(e) || e.fresh) continue;
        if (e.needed < oldest) {
            oldest = e.needed;
            best = static_cast<int>(p);
        }
    }
    if (best < 0) return -1;
    release(owner_[static_cast<size_t>(best)]);
    free_.pop_back();   // (release gave it back)
    ++stats_.evicted;
    return best;
}

void ShadowPages::grant(std::span<const uint64_t> keys) { granted_.insert(granted_.end(), keys.begin(), keys.end()); }

void ShadowPages::viewMatrix(const Map& m, const Rect& r, ShadowViewPlan& v) const {
    const int n = m.kind == SP_KIND_CLIPMAP ? SP_WINDOW : SP_LOCAL >> r.mip;
    const double total = static_cast<double>(n) * SP_PAGE;
    const double u0 = (r.x * SP_PAGE - SP_BORDER) / total, u1 = ((r.x + r.w) * SP_PAGE + SP_BORDER) / total;
    const double v0 = (r.y * SP_PAGE - SP_BORDER) / total, v1 = ((r.y + r.h) * SP_PAGE + SP_BORDER) / total;
    // (u, v) are Vulkan's: v runs down, against bromath's clip y.
    const double x0 = 2.0 * u0 - 1.0, x1 = 2.0 * u1 - 1.0;
    const double y0 = 1.0 - 2.0 * v1, y1 = 1.0 - 2.0 * v0;
    bromath::Mat4 crop = bromath::midentity();
    crop.at(0, 0) = static_cast<float>(2.0 / (x1 - x0));
    crop.at(0, 3) = static_cast<float>(-(x1 + x0) / (x1 - x0));
    crop.at(1, 1) = static_cast<float>(2.0 / (y1 - y0));
    crop.at(1, 3) = static_cast<float>(-(y1 + y0) / (y1 - y0));
    v.relViewProj = bromath::mmul(crop, m.relViewProj);
    v.frustum = makeFrustum(bromath::mmul(crop, m.viewProj));
    v.texelsW = r.w * SP_PAGE + 2 * SP_BORDER;
    v.texelsH = r.h * SP_PAGE + 2 * SP_BORDER;
    v.ortho = m.ortho;
    v.texel = m.kind == SP_KIND_CLIPMAP ? m.texel : 0.0f;
    v.texelPerMetre = m.kind == SP_KIND_LOCAL ? std::ldexp(m.texelPerMetre, r.mip) : 0.0f;
}

void ShadowPages::plan(ShadowPlan& out) {
    stats_ = {};
    out.origin = eye_;
    out.serial = serial_;
    out.mapCount = 0;
    for (int i = 0; i < ShadowPlan::kMaxLights; ++i) {
        out.lightMap[i] = -1;
        out.lightMapCount[i] = 0;
        out.lightReach[i] = 0.0f;
    }
    out.finestTexel = config_.finestTexel;
    out.levels = config_.levels;
    out.swayLevel = swayLevel();
    out.searchLevel = searchLevel();
    out.stillPenumbra = config_.stillPenumbra;
    out.poolColumns = config_.poolColumns;
    out.poolRows = config_.poolRows;
    out.scratchSize = config_.scratchSize;
    out.viewCount = 0;
    out.copyOut.clear();
    out.staticPages = 0;
    out.plantPages = 0;
    out.plantUnits = 0.0;
    out.nextCount = 0;
    out.entryCount = entryCount();
    // (The table ends before the clipmap built: nothing samples it.)
    out.table.assign(static_cast<size_t>(clipFirst(true)) * 2, 0u);

    for (const Map& m : maps_) {
        if (m.next ? out.nextCount >= SP_MAX_LEVELS : out.mapCount >= SP_MAX_MAPS) continue;
        ShadowMapPlan& p = m.next ? out.next[out.nextCount] : out.maps[out.mapCount];
        p.relViewProj = m.relViewProj;
        p.viewProj = m.viewProj;
        p.kind = m.kind;
        p.light = m.light;
        p.level = m.kind == SP_KIND_CLIPMAP ? m.level : 0;
        p.firstEntry = m.first;
        p.originX = m.kind == SP_KIND_CLIPMAP ? static_cast<int>(wrap(sunOf(m.next).ox[m.level])) : 0;
        p.originY = m.kind == SP_KIND_CLIPMAP ? static_cast<int>(wrap(sunOf(m.next).oy[m.level])) : 0;
        p.texel = m.texel;
        p.texelPerMetre = m.texelPerMetre;
        p.zNear = m.zNear;
        p.zFar = m.zFar;
        p.ortho = m.ortho;
        p.bias = m.bias;
        p.normalBias = m.normalBias;
        if (m.next) {
            ++out.nextCount;
            continue;
        }
        if (m.light >= 0 && m.light < ShadowPlan::kMaxLights) {
            if (out.lightMap[m.light] < 0) out.lightMap[m.light] = out.mapCount;
            ++out.lightMapCount[m.light];
            if (m.kind == SP_KIND_CLIPMAP) out.lightReach[m.light] = sun_.reach;
        }
        ++out.mapCount;
    }
    if (maps_.empty()) {
        wants_.clear();
        granted_.clear();
        return;
    }

    spreadLoads();

    // The overlays: the pages what moves touches that want one, the finest
    // first while overlayPages allows.
    markBase();
    markDynamic();
    std::vector<int> dynamicPages;
    for (const Map& m : maps_) {
        if (m.next) continue;
        const int count = m.kind == SP_KIND_CLIPMAP ? SP_LEVEL_ENTRIES : SP_LOCAL_ENTRIES;
        for (int i = m.first; i < m.first + count; ++i)
            if (entries_[static_cast<size_t>(i)].dynamic) dynamicPages.push_back(i);
    }
    if (static_cast<int>(dynamicPages.size()) > config_.overlayPages) {
        // (Clipmap entries run finest level first, a local map's finest mip first.)
        for (size_t k = static_cast<size_t>(config_.overlayPages); k < dynamicPages.size(); ++k)
            entries_[static_cast<size_t>(dynamicPages[k])].dynamic = false;
        dynamicPages.resize(static_cast<size_t>(config_.overlayPages));
    }

    // The base pages missing or stale (drawn now: they are what shows while
    // a page waits), then the world service's grants of last frame still
    // wanted, each with a physical page.
    std::vector<int> drawPages;
    std::vector<uint8_t> chosen(entries_.size(), 0);
    int mip = 0;
    // A stale needed page is redrawn now too, while staleNow allows (finest
    // first): it shows a caster where it no longer is, not merely a coarser
    // texel. (The clipmap built shows nothing: its pages wait for grants.)
    std::vector<uint64_t> keys;
    int stale = 0;
    for (size_t i = 0; i < static_cast<size_t>(clipFirst(true)); ++i) {
        const Entry& e = entries_[i];
        if (e.base == serial_ && (!e.drawn || e.dirty)) keys.push_back(i);
        else if (e.drawn && e.dirty && neededNow(e) && stale < config_.staleNow) {
            keys.push_back(i);
            ++stale;
        }
    }
    keys.insert(keys.end(), granted_.begin(), granted_.end());
    for (const uint64_t key : keys) {
        if (key >= entries_.size()) continue;
        const int i = static_cast<int>(key);
        Entry& e = entries_[key];
        if (chosen[key] || !e.occupied || mapOfEntry(i, mip) < 0 || !neededNow(e) || (e.drawn && !e.dirty)) continue;
        if (e.physical < 0) {
            e.fresh = true;   // (kept from eviction while the frame allocates)
            const int p = allocate();
            if (p < 0) {
                e.fresh = false;
                continue;
            }
            e.physical = p;
            owner_[static_cast<size_t>(p)] = i;
            e.drawn = false;
        }
        chosen[key] = 1;
        drawPages.push_back(i);
    }
    granted_.clear();

    // Rects: a group a (map, mip, overlay or not), each its grid's cells
    // covered exactly, coarser blocks where the frame's views run out.
    struct Group {
        int map, mip, n;
        bool dynamic;
        std::vector<uint8_t> grid;
        std::vector<int> rects;
        int block = 1;
    };
    std::vector<Group> groups;
    const auto cellOf = [&](int i, int& map, int& m, int& x, int& y, int& n) {
        map = mapOfEntry(i, m);
        const Entry& e = entries_[static_cast<size_t>(i)];
        if (maps_[static_cast<size_t>(map)].kind == SP_KIND_CLIPMAP) {
            const int level = maps_[static_cast<size_t>(map)].level;
            const Sun& sun = sunOf(maps_[static_cast<size_t>(map)].next);
            x = static_cast<int>(e.x - sun.ox[level]);
            y = static_cast<int>(e.y - sun.oy[level]);
            n = SP_WINDOW;
        } else {
            x = static_cast<int>(e.x);
            y = static_cast<int>(e.y);
            n = SP_LOCAL >> m;
        }
    };
    const auto addPages = [&](const std::vector<int>& pages, bool dynamic) {
        for (const int i : pages) {
            int map, m, x, y, n;
            cellOf(i, map, m, x, y, n);
            Group* g = nullptr;
            for (Group& h : groups)
                if (h.map == map && h.mip == m && h.dynamic == dynamic) g = &h;
            if (!g) {
                groups.push_back({map, m, n, dynamic, std::vector<uint8_t>(static_cast<size_t>(n) * n, 0), {}, 1});
                g = &groups.back();
            }
            g->grid[static_cast<size_t>(y) * n + x] = 1;
        }
    };
    addPages(dynamicPages, true);
    addPages(drawPages, false);
    // An overlay group is one rect, its pages' bounds, where that fits: a
    // view's cost is mostly its casters' (each plant drawn once a view it
    // touches), not its texels.
    const int maxSide = std::max(1, (config_.scratchSize - 2 * SP_BORDER) / SP_PAGE);
    int total = 0;
    for (Group& g : groups) {
        int x0 = g.n, y0 = g.n, x1 = -1, y1 = -1;
        if (g.dynamic)
            for (int y = 0; y < g.n; ++y)
                for (int x = 0; x < g.n; ++x)
                    if (g.grid[static_cast<size_t>(y) * g.n + x]) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
        if (g.dynamic && x1 - x0 < maxSide && y1 - y0 < maxSide)
            g.rects = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        else
            shadowCoverRects(g.grid, g.n, g.n, maxSide, 1, g.rects);
        total += static_cast<int>(g.rects.size() / 4);
    }
    while (total > SP_MAX_VIEWS) {
        Group* worst = nullptr;
        for (Group& g : groups)
            if (g.block < g.n && g.rects.size() > 4 && (!worst || g.rects.size() > worst->rects.size())) worst = &g;
        if (!worst) break;
        total -= static_cast<int>(worst->rects.size() / 4);
        worst->block *= 2;
        shadowCoverRects(worst->grid, worst->n, worst->n, maxSide, worst->block, worst->rects);
        total += static_cast<int>(worst->rects.size() / 4);
    }

    // Packed into the scratch target in shelves, the overlays first; a rect
    // that does not fit waits (its pages stay wanted, its overlays' pages
    // show their cached depth alone).
    std::vector<Rect> rects;
    for (const Group& g : groups)
        for (size_t k = 0; k + 3 < g.rects.size(); k += 4)
            rects.push_back({g.map, g.mip, g.rects[k], g.rects[k + 1], g.rects[k + 2], g.rects[k + 3], g.dynamic});
    std::stable_sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) {
        if (a.dynamic != b.dynamic) return a.dynamic;
        return a.h > b.h;
    });
    std::vector<uint8_t> inView(entries_.size(), 0);
    int shelfX = 0, shelfY = 0, shelfH = 0;
    const int scratch = config_.scratchSize;
    for (const Rect& r : rects) {
        if (out.viewCount >= SP_MAX_VIEWS) break;
        const int tw = r.w * SP_PAGE + 2 * SP_BORDER, th = r.h * SP_PAGE + 2 * SP_BORDER;
        if (shelfX + tw > scratch) {
            shelfY += shelfH;
            shelfX = 0;
            shelfH = 0;
        }
        if (shelfY + th > scratch || tw > scratch) continue;
        const Map& m = maps_[static_cast<size_t>(r.map)];
        ShadowViewPlan& v = out.views[out.viewCount++];
        v.map = r.map;
        v.mip = r.mip;
        v.x = r.x;
        v.y = r.y;
        v.w = r.w;
        v.h = r.h;
        v.dynamic = r.dynamic;
        v.scratchX = shelfX;
        v.scratchY = shelfY;
        viewMatrix(m, r, v);
        if (m.kind == SP_KIND_CLIPMAP) {
            // What sways: the sway level's overlays, the cached depth past
            // it; at the sway level and finer the cached depth holds the
            // crowns whose penumbra hides their sway.
            const int s = swayLevel();
            if (r.dynamic) v.plants = m.level == s ? ShadowPlants::Swaying : ShadowPlants::None;
            else v.plants = m.level > s ? ShadowPlants::All : ShadowPlants::Still;
            v.penumbra = sunOf(m.next).penumbra;
            v.grass = r.dynamic && m.level == s;
        } else {
            v.plants = r.dynamic ? ShadowPlants::All : ShadowPlants::None;
            v.grass = r.dynamic;
        }
        shelfX += tw;
        shelfH = std::max(shelfH, th);
        // Its pages: the group's cells inside it.
        const int n = m.kind == SP_KIND_CLIPMAP ? SP_WINDOW : SP_LOCAL >> r.mip;
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) {
                int i;
                if (m.kind == SP_KIND_CLIPMAP)
                    i = m.first + static_cast<int>(wrap(sunOf(m.next).oy[m.level] + y) * SP_WINDOW +
                                                   wrap(sunOf(m.next).ox[m.level] + x));
                else
                    i = m.first + SP_LOCAL_MIP_FIRST(r.mip) + y * n + x;
                Entry& e = entries_[static_cast<size_t>(i)];
                // (An overlay rect's every page has its overlay drawn.)
                const bool mine = r.dynamic ? (m.kind != SP_KIND_CLIPMAP || e.occupied) : chosen[static_cast<size_t>(i)] != 0;
                if (!mine || inView[static_cast<size_t>(i)] & (r.dynamic ? 1 : 2)) continue;
                inView[static_cast<size_t>(i)] |= r.dynamic ? 1 : 2;
                ShadowPageCopy c;
                c.scratchX = v.scratchX + (x - r.x) * SP_PAGE;
                c.scratchY = v.scratchY + (y - r.y) * SP_PAGE;
                if (r.dynamic) {
                    // Sampled in place: its page's first texel (past the border).
                    e.overlay = SP_RESIDENT | static_cast<uint32_t>(c.scratchX + SP_BORDER) |
                                (static_cast<uint32_t>(c.scratchY + SP_BORDER) << SP_OVERLAY_BITS);
                    ++stats_.dynamic;
                    continue;
                }
                c.physical = e.physical;
                c.plants = v.plants == ShadowPlants::All;
                ++out.staticPages;
                out.plantPages += c.plants;
                if (c.plants) out.plantUnits += plantUnits(i);
                out.copyOut.push_back(c);
            }
    }
    stats_.views = static_cast<uint32_t>(out.viewCount);
    // What did not fit: no overlay; a page granted and not drawn gives its
    // physical page back if it had nothing in it.
    for (const int i : dynamicPages)
        if (!(inView[static_cast<size_t>(i)] & 1)) entries_[static_cast<size_t>(i)].dynamic = false;
    for (const int i : drawPages) {
        Entry& e = entries_[static_cast<size_t>(i)];
        if (inView[static_cast<size_t>(i)] & 2) continue;
        e.fresh = false;
        if (!e.drawn) release(i);
    }

    // The page table: this frame's draws (in the pool before the lit
    // passes sample it), the pages that held depth before and the overlays.
    for (const int i : drawPages) {
        Entry& e = entries_[static_cast<size_t>(i)];
        if (!(inView[static_cast<size_t>(i)] & 2)) continue;
        e.fresh = !e.drawn;
        e.drawn = true;
        e.dirty = false;
        ++stats_.drawn;
    }
    for (const Map& m : maps_) {
        if (m.next) continue;
        const int count = m.kind == SP_KIND_CLIPMAP ? SP_LEVEL_ENTRIES : SP_LOCAL_ENTRIES;
        for (int i = m.first; i < m.first + count; ++i) {
            const Entry& e = entries_[static_cast<size_t>(i)];
            const bool needed = neededNow(e);
            stats_.needed += needed;
            out.table[2 * static_cast<size_t>(i) + 1] = e.overlay;
            // (A stale page no one needs is not sampled: a coarser one is.)
            if (!e.drawn || e.physical < 0 || (e.dirty && !needed)) continue;
            out.table[2 * static_cast<size_t>(i)] = SP_RESIDENT | static_cast<uint32_t>(e.physical);
            stats_.resident += needed;
        }
    }

    // The world service's jobs: every needed page not drawn or stale. The
    // clipmap built's are under a pixel: what the shown one leaves.
    wants_.clear();
    uint32_t building = 0;
    const int sway = swayLevel();
    for (size_t mi = 0; mi < maps_.size(); ++mi) {
        const Map& m = maps_[mi];
        const bool clip = m.kind == SP_KIND_CLIPMAP;
        const int count = clip ? SP_LEVEL_ENTRIES : SP_LOCAL_ENTRIES;
        for (int i = m.first; i < m.first + count; ++i) {
            const Entry& e = entries_[static_cast<size_t>(i)];
            if (!neededNow(e) || (e.drawn && !e.dirty)) continue;
            int level = clip ? m.level : 0;
            if (!clip) mapOfEntry(i, level);
            const int top = clip ? config_.levels - 1 : SP_LOCAL_MIPS - 1;
            // Its ancestors: the nearest drawn (what shows in its place) and
            // whether any is needed (else this is the top of its chain).
            int shown = -1;
            bool neededAbove = false;
            for (int k = 1; level + k <= top; ++k) {
                int a;
                if (clip) {
                    a = clipmapEntry(level + k, e.x >> k, e.y >> k, m.next);
                } else {
                    const int n = SP_LOCAL >> (level + k);
                    a = m.first + SP_LOCAL_MIP_FIRST(level + k) + static_cast<int>((e.y >> k) * n + (e.x >> k));
                }
                if (a < 0) continue;
                const Entry& anc = entries_[static_cast<size_t>(a)];
                if (shown < 0 && anc.drawn) shown = k;
                neededAbove = neededAbove || neededNow(anc);
            }
            ShadowPageWant w;
            w.key = static_cast<uint64_t>(i);
            w.error = e.drawn ? 1.0f : shown > 0 ? std::min(std::ldexp(1.0f, shown), 64.0f) : 64.0f;
            w.size = std::ldexp(1.0, level);
            w.required = !e.drawn && !neededAbove;
            w.plants = clip && level > sway;
            if (w.plants) w.units = plantUnits(i);
            if (m.next) {
                w.error = std::min(w.error, kNextError);
                w.required = false;
                ++building;
            }
            wants_.push_back(w);
        }
    }
    stats_.wanted = static_cast<uint32_t>(wants_.size()) - building;
    stats_.building = building;
    // The clipmap built is ready once requests have named its pages and
    // every one needed is drawn.
    ready_ = building_ && newestRequest_ >= begun_ && building == 0;
}

}  // namespace bro::scene
