#include "scene/world/shadow_pages.h"

#include "scene/depth_policy.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace bro::scene {

namespace {

int64_t wrap(int64_t v) { return ((v % SP_WINDOW) + SP_WINDOW) % SP_WINDOW; }

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void corners(const bromath::AABB3& box, double out[8][3]) {
    for (int c = 0; c < 8; ++c) {
        out[c][0] = (c & 1) ? box.max.x : box.min.x;
        out[c][1] = (c & 2) ? box.max.y : box.min.y;
        out[c][2] = (c & 4) ? box.max.z : box.min.z;
    }
}

int mipFirst(int m) { return SP_LOCAL_MIP_FIRST(m); }

// `m` (column-major) times (p, 1).
void clipOf(const bromath::Mat4& m, const double p[3], double c[4]) {
    for (int r = 0; r < 4; ++r) c[r] = m.at(r, 0) * p[0] + m.at(r, 1) * p[1] + m.at(r, 2) * p[2] + m.at(r, 3);
}

}  // namespace

ShadowPages::ShadowPages() { configure({}); }

void ShadowPages::configure(const ShadowPagesConfig& config) {
    config_ = config;
    config_.levels = std::clamp(config_.levels, 1, SP_MAX_LEVELS);
    const int total = std::max(2, config_.poolColumns * config_.poolRows);
    config_.overlayPages = std::max(config_.overlayPages, 0);
    entries_.assign(static_cast<size_t>(config_.levels) * SP_LEVEL_ENTRIES + kLocalSlots * SP_LOCAL_ENTRIES, Entry{});
    owner_.assign(static_cast<size_t>(total), -1);
    free_.clear();
    for (int p = total - 1; p >= 0; --p) free_.push_back(p);
    sun_ = Sun{};
    for (LocalSlot& s : locals_) s = LocalSlot{};
    for (History& h : history_) h = History{};
    maps_.clear();
    dynamic_.clear();
    granted_.clear();
    wants_.clear();
    newestRequest_ = 0;
    stats_ = {};
}

// --- Light space ---

float ShadowPages::levelTexel(int level) const { return std::ldexp(config_.finestTexel, level); }

double ShadowPages::pageSize(int level) const { return static_cast<double>(levelTexel(level)) * SP_PAGE; }

int64_t ShadowPages::pageOf(double coordinate, double pageSize) {
    return static_cast<int64_t>(std::floor(coordinate / pageSize));
}

void ShadowPages::sunPlane(const double p[3], double& a, double& b, double& c) const {
    const double q[3] = {p[0] - sun_.anchor[0], p[1] - sun_.anchor[1], p[2] - sun_.anchor[2]};
    a = dot3(sun_.s, q) + sun_.offset[0];
    b = -dot3(sun_.u, q) + sun_.offset[1];
    c = dot3(sun_.d, q) + sun_.offset[2];
}

void ShadowPages::sunCoordinates(const bromath::Vec3& p, double& a, double& b, double& c) const {
    const double q[3] = {p.x, p.y, p.z};
    sunPlane(q, a, b, c);
}

void ShadowPages::window(int level, int64_t& x, int64_t& y) const {
    x = sun_.ox[level];
    y = sun_.oy[level];
}

int ShadowPages::clipmapEntry(int level, int64_t x, int64_t y) const {
    if (level < 0 || level >= config_.levels) return -1;
    if (x < sun_.ox[level] || x >= sun_.ox[level] + SP_WINDOW || y < sun_.oy[level] || y >= sun_.oy[level] + SP_WINDOW)
        return -1;
    return level * SP_LEVEL_ENTRIES + static_cast<int>(wrap(y) * SP_WINDOW + wrap(x));
}

ShadowPages::EntryView ShadowPages::entry(int index) const {
    EntryView v;
    if (index < 0 || index >= entryCount()) return v;
    const Entry& e = entries_[static_cast<size_t>(index)];
    v.x = e.x;
    v.y = e.y;
    v.physical = e.physical;
    v.drawn = e.drawn;
    v.dirty = e.dirty;
    v.needed = neededNow(e);
    return v;
}

int ShadowPages::mapOfEntry(int index, int& mip) const {
    mip = 0;
    const int clip = config_.levels * SP_LEVEL_ENTRIES;
    if (index < clip) {
        const int level = index / SP_LEVEL_ENTRIES;
        return sun_.on && level < static_cast<int>(maps_.size()) ? level : -1;
    }
    const int slot = (index - clip) / SP_LOCAL_ENTRIES;
    const int j = (index - clip) % SP_LOCAL_ENTRIES;
    mip = j >= 84 ? 3 : j >= 80 ? 2 : j >= 64 ? 1 : 0;
    for (size_t m = 0; m < maps_.size(); ++m)
        if (maps_[m].kind == SP_KIND_LOCAL && maps_[m].level == slot) return static_cast<int>(m);
    return -1;
}

// --- A frame's lights ---

void ShadowPages::beginFrame(const bromath::Vec3& eye) {
    ++serial_;
    eye_ = eye;
    maps_.clear();
    dynamic_.clear();
    lightsSet_ = false;
    invalid_.clear();
    sun_.on = false;
    for (LocalSlot& s : locals_) s.seen = false;
    for (Entry& e : entries_) {
        e.fresh = false;
        e.dynamic = false;
        e.overlay = 0;
    }
}

void ShadowPages::release(int index) {
    Entry& e = entries_[static_cast<size_t>(index)];
    if (e.physical >= 0) {
        owner_[static_cast<size_t>(e.physical)] = -1;
        free_.push_back(e.physical);
    }
    e.physical = -1;
    e.drawn = false;
    e.dirty = false;
}

void ShadowPages::releaseMap(int first, int count) {
    for (int i = first; i < first + count; ++i) {
        release(i);
        entries_[static_cast<size_t>(i)].needed = 0;
        entries_[static_cast<size_t>(i)].occupied = false;
    }
}

void ShadowPages::moveWindows() {
    const double eye[3] = {eye_.x, eye_.y, eye_.z};
    double ea, eb, ec;
    sunPlane(eye, ea, eb, ec);
    for (int level = 0; level < config_.levels; ++level) {
        const double size = pageSize(level);
        const int64_t ox = pageOf(ea, size) - SP_WINDOW / 2;
        const int64_t oy = pageOf(eb, size) - SP_WINDOW / 2;
        if (sun_.haveWindows && ox == sun_.ox[level] && oy == sun_.oy[level]) continue;
        sun_.ox[level] = ox;
        sun_.oy[level] = oy;
        // Each slot holds the one page of the window congruent to it: a page
        // the window left gives its slot (and its physical page) to the one
        // that came in.
        for (int sy = 0; sy < SP_WINDOW; ++sy)
            for (int sx = 0; sx < SP_WINDOW; ++sx) {
                const int64_t x = ox + wrap(sx - ox), y = oy + wrap(sy - oy);
                const int i = level * SP_LEVEL_ENTRIES + sy * SP_WINDOW + sx;
                Entry& e = entries_[static_cast<size_t>(i)];
                if (e.occupied && e.x == x && e.y == y) continue;
                release(i);
                e.occupied = true;
                e.x = x;
                e.y = y;
                e.needed = 0;
            }
    }
    sun_.haveWindows = true;
}

void ShadowPages::setSun(const ShadowSun& sun) {
    double d[3] = {sun.direction.x, sun.direction.y, sun.direction.z};
    const double len = std::sqrt(dot3(d, d));
    if (len < 1e-9) return;
    for (double& v : d) v /= len;
    const int clip = config_.levels * SP_LEVEL_ENTRIES;
    const double eye[3] = {eye_.x, eye_.y, eye_.z};
    const auto basis = [&] {
        // mlookAt's: across = normalize(d x up), up' = across x d.
        const double up[3] = {0.0, std::fabs(d[1]) > 0.99 ? 0.0 : 1.0, std::fabs(d[1]) > 0.99 ? 1.0 : 0.0};
        double s[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
        const double sl = std::sqrt(dot3(s, s));
        for (double& v : s) v /= sl;
        const double u[3] = {s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0]};
        std::memcpy(sun_.s, s, sizeof(s));
        std::memcpy(sun_.u, u, sizeof(u));
        std::memcpy(sun_.d, d, sizeof(d));
    };
    if (!sun_.had || sun.lightId != sun_.lightId) {
        // Another light's pages: nothing of them holds.
        releaseMap(0, clip);
        sun_ = Sun{};
        basis();
        std::memcpy(sun_.anchor, eye, sizeof(eye));
        sun_.had = true;
        sun_.lightId = sun.lightId;
    } else if (dot3(sun_.d, d) < std::cos(static_cast<double>(config_.sunTurn))) {
        // The sun turned: a new basis about the eye, its plane coordinates
        // there kept, so the pages near the eye move least while every page
        // waits to be redrawn (and draws, stale, until it is).
        double a, b, c;
        sunPlane(eye, a, b, c);
        basis();
        std::memcpy(sun_.anchor, eye, sizeof(eye));
        sun_.offset[0] = a;
        sun_.offset[1] = b;
        sun_.offset[2] = c;
        sun_.haveRange = false;
        for (int i = 0; i < clip; ++i)
            if (entries_[static_cast<size_t>(i)].drawn) entries_[static_cast<size_t>(i)].dirty = true;
    }
    sun_.on = true;
    sun_.lightIndex = sun.lightIndex;
    sun_.bias = sun.bias;
    sun_.normalBias = sun.normalBias;
    sun_.reach = sun.reach;

    // The depth range along the light: the casters' extent, grown in coarse
    // steps (a change redraws every page, so it moves rarely) and never
    // shrunk.
    sun_.casterRect[0] = sun_.casterRect[2] = 1e300;
    sun_.casterRect[1] = sun_.casterRect[3] = -1e300;
    if (!bromath::aisEmpty(sun.casters)) {
        double cs[8][3];
        corners(sun.casters, cs);
        double lo = 1e300, hi = -1e300;
        for (auto& p : cs) {
            double a, b, c;
            sunPlane(p, a, b, c);
            lo = std::min(lo, c);
            hi = std::max(hi, c);
            sun_.casterRect[0] = std::min(sun_.casterRect[0], a);
            sun_.casterRect[1] = std::max(sun_.casterRect[1], a);
            sun_.casterRect[2] = std::min(sun_.casterRect[2], b);
            sun_.casterRect[3] = std::max(sun_.casterRect[3], b);
        }
        if (!sun_.haveRange || lo < sun_.zLo || hi > sun_.zHi) {
            if (sun_.haveRange) {
                lo = std::min(lo, sun_.zLo);
                hi = std::max(hi, sun_.zHi);
            }
            const double step = std::exp2(std::ceil(std::log2(std::max(16.0, (hi - lo) * 0.25))));
            const double newLo = std::floor((lo - step * 0.5) / step) * step;
            const double newHi = std::ceil((hi + step * 0.5) / step) * step;
            if (sun_.haveRange)
                for (int i = 0; i < clip; ++i)
                    if (entries_[static_cast<size_t>(i)].drawn) entries_[static_cast<size_t>(i)].dirty = true;
            sun_.zLo = newLo;
            sun_.zHi = newHi;
            sun_.haveRange = true;
        }
    }
    if (!sun_.haveRange) {
        sun_.zLo = -64.0;
        sun_.zHi = 64.0;
    }
    moveWindows();

    // A level's map: an ortho window over its pages, relative to the eye.
    double ea, eb, ec;
    sunPlane(eye, ea, eb, ec);
    bromath::Mat4 rot = bromath::midentity();
    for (int c = 0; c < 3; ++c) {
        rot.at(0, c) = static_cast<float>(sun_.s[c]);
        rot.at(1, c) = static_cast<float>(sun_.u[c]);
        rot.at(2, c) = static_cast<float>(-sun_.d[c]);
    }
    const float zNear = static_cast<float>(sun_.zLo - ec), zFar = static_cast<float>(sun_.zHi - ec);
    for (int level = 0; level < config_.levels; ++level) {
        const double size = pageSize(level), w = size * SP_WINDOW;
        const double a0 = static_cast<double>(sun_.ox[level]) * size - ea;
        const double b0 = static_cast<double>(sun_.oy[level]) * size - eb;
        Map m;
        m.kind = SP_KIND_CLIPMAP;
        m.level = level;
        m.first = level * SP_LEVEL_ENTRIES;
        m.light = sun.lightIndex;
        m.lightId = sun.lightId;
        m.relViewProj = bromath::mmul(makeOrthoZeroToOne(static_cast<float>(a0), static_cast<float>(a0 + w),
                                                         static_cast<float>(-(b0 + w)), static_cast<float>(-b0),
                                                         zNear, zFar),
                                      rot);
        m.viewProj = bromath::mmul(m.relViewProj, bromath::mtranslate(-eye_));
        m.texel = levelTexel(level);
        m.zNear = zNear;
        m.zFar = zFar;
        m.ortho = true;
        m.bias = sun.bias;
        m.normalBias = sun.normalBias;
        maps_.push_back(m);
    }
}

bool ShadowPages::addLocal(const ShadowLocal& local) {
    // Its faces' slots: the ones it held, else free ones.
    int slots[6];
    int found = 0;
    for (int f = 0; f < local.faces; ++f) {
        slots[f] = -1;
        for (int s = 0; s < kLocalSlots; ++s)
            if (locals_[s].used && locals_[s].lightId == local.lightId && locals_[s].face == f) slots[f] = s;
    }
    for (int f = 0; f < local.faces; ++f) {
        if (slots[f] >= 0) {
            ++found;
            continue;
        }
        for (int s = 0; s < kLocalSlots && slots[f] < 0; ++s) {
            bool taken = locals_[s].used;
            for (int g = 0; g < f && !taken; ++g) taken = slots[g] == s;
            if (!taken) slots[f] = s;
        }
        if (slots[f] >= 0) ++found;
    }
    if (found < local.faces || static_cast<int>(maps_.size()) + local.faces > SP_MAX_MAPS) return false;
    for (int f = 0; f < local.faces; ++f) {
        LocalSlot& slot = locals_[slots[f]];
        const int first = localFirst(slots[f]);
        if (!slot.used) {
            releaseMap(first, SP_LOCAL_ENTRIES);
            slot.used = true;
            slot.lightId = local.lightId;
            slot.face = f;
            slot.viewProj = local.viewProj[f];
            for (int j = 0; j < SP_LOCAL_ENTRIES; ++j) {
                Entry& e = entries_[static_cast<size_t>(first + j)];
                const int mip = j >= 84 ? 3 : j >= 80 ? 2 : j >= 64 ? 1 : 0;
                const int n = SP_LOCAL >> mip, k = j - mipFirst(mip);
                e.x = k % n;
                e.y = k / n;
                e.occupied = true;
            }
        } else if (std::memcmp(slot.viewProj.data, local.viewProj[f].data, sizeof(slot.viewProj.data)) != 0) {
            // The light moved: its pages wait to be redrawn.
            slot.viewProj = local.viewProj[f];
            for (int j = 0; j < SP_LOCAL_ENTRIES; ++j)
                if (entries_[static_cast<size_t>(first + j)].drawn) entries_[static_cast<size_t>(first + j)].dirty = true;
        }
        slot.seen = true;
        Map m;
        m.kind = SP_KIND_LOCAL;
        m.level = slots[f];
        m.first = first;
        m.light = local.lightIndex;
        m.lightId = local.lightId;
        m.face = f;
        m.relViewProj = local.relViewProj[f];
        m.viewProj = local.viewProj[f];
        m.texelPerMetre = local.texelPerMetre;
        m.zNear = local.zNear;
        m.zFar = local.zFar;
        m.ortho = false;
        m.bias = local.bias;
        m.normalBias = local.normalBias;
        maps_.push_back(m);
    }
    return true;
}

void ShadowPages::endLights() {
    for (int s = 0; s < kLocalSlots; ++s) {
        if (!locals_[s].used || locals_[s].seen) continue;
        releaseMap(localFirst(s), SP_LOCAL_ENTRIES);
        locals_[s].used = false;
    }
    // The casters' changes met before the maps were set.
    lightsSet_ = true;
    for (const bromath::AABB3& box : invalid_) invalidate(box);
    invalid_.clear();
    History& h = history_[serial_ % 8];
    h.serial = serial_;
    h.sun = sun_.on;
    std::memcpy(h.ox, sun_.ox, sizeof(h.ox));
    std::memcpy(h.oy, sun_.oy, sizeof(h.oy));
    for (int s = 0; s < kLocalSlots; ++s) {
        h.localId[s] = locals_[s].used ? locals_[s].lightId : 0u;
        h.localFace[s] = locals_[s].face;
    }
}

// --- Casters ---

void ShadowPages::planeRect(const bromath::AABB3& box, double rect[4]) const {
    double cs[8][3];
    corners(box, cs);
    rect[0] = rect[2] = 1e300;
    rect[1] = rect[3] = -1e300;
    for (auto& p : cs) {
        double a, b, c;
        sunPlane(p, a, b, c);
        rect[0] = std::min(rect[0], a);
        rect[1] = std::max(rect[1], a);
        rect[2] = std::min(rect[2], b);
        rect[3] = std::max(rect[3], b);
    }
}

template <class F>
void ShadowPages::forLevelRect(int level, const double rect[4], F&& f) {
    const Map& m = maps_[static_cast<size_t>(level)];
    const double size = pageSize(level), border = static_cast<double>(m.texel) * SP_BORDER;
    const int64_t ox = sun_.ox[level], oy = sun_.oy[level];
    const auto clampPage = [&](double v, int64_t o) {
        const double p = std::floor(v / size);
        return static_cast<int64_t>(std::clamp(p, static_cast<double>(o), static_cast<double>(o + SP_WINDOW - 1)));
    };
    if (rect[1] + border < ox * size || rect[0] - border >= (ox + SP_WINDOW) * size ||
        rect[3] + border < oy * size || rect[2] - border >= (oy + SP_WINDOW) * size)
        return;
    const int64_t x0 = clampPage(rect[0] - border, ox), x1 = clampPage(rect[1] + border, ox);
    const int64_t y0 = clampPage(rect[2] - border, oy), y1 = clampPage(rect[3] + border, oy);
    for (int64_t y = y0; y <= y1; ++y)
        for (int64_t x = x0; x <= x1; ++x) f(m.first + static_cast<int>(wrap(y) * SP_WINDOW + wrap(x)));
}

template <class F>
void ShadowPages::forPages(const bromath::AABB3& box, int minLevel, int maxLevel, F&& f) {
    if (bromath::aisEmpty(box)) return;
    double cs[8][3];
    corners(box, cs);
    double rect[4];
    if (sun_.on) planeRect(box, rect);
    for (const Map& m : maps_) {
        if (m.kind == SP_KIND_CLIPMAP) {
            if (m.level >= minLevel && m.level <= maxLevel) forLevelRect(m.level, rect, f);
        } else {
            // The box's corners in the face's (u, v); a corner behind the
            // light covers the face.
            double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
            bool whole = false;
            int left = 0, right = 0, below = 0, above = 0;
            for (auto& p : cs) {
                double c[4];
                clipOf(m.viewProj, p, c);
                if (c[3] <= 1e-6) {
                    whole = true;
                    break;
                }
                const double u = 0.5 * c[0] / c[3] + 0.5, v = 0.5 - 0.5 * c[1] / c[3];
                u0 = std::min(u0, u);
                u1 = std::max(u1, u);
                v0 = std::min(v0, v);
                v1 = std::max(v1, v);
                left += u < 0.0;
                right += u > 1.0;
                below += v < 0.0;
                above += v > 1.0;
            }
            if (!whole && (left == 8 || right == 8 || below == 8 || above == 8)) continue;
            if (whole) u0 = v0 = 0.0, u1 = v1 = 1.0;
            for (int mip = 0; mip < SP_LOCAL_MIPS; ++mip) {
                const int n = SP_LOCAL >> mip;
                const double border = static_cast<double>(SP_BORDER) / (n * SP_PAGE);
                const int x0 = std::clamp(static_cast<int>(std::floor((u0 - border) * n)), 0, n - 1);
                const int x1 = std::clamp(static_cast<int>(std::floor((u1 + border) * n)), 0, n - 1);
                const int y0 = std::clamp(static_cast<int>(std::floor((v0 - border) * n)), 0, n - 1);
                const int y1 = std::clamp(static_cast<int>(std::floor((v1 + border) * n)), 0, n - 1);
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) f(m.first + mipFirst(mip) + y * n + x);
            }
        }
    }
}

void ShadowPages::invalidate(const bromath::AABB3& box) {
    if (!lightsSet_) {
        invalid_.push_back(box);
        return;
    }
    forPages(box, 0, SP_MAX_LEVELS, [this](int i) {
        Entry& e = entries_[static_cast<size_t>(i)];
        if (e.drawn) e.dirty = true;
    });
}

void ShadowPages::invalidateAll() {
    for (Entry& e : entries_)
        if (e.drawn) e.dirty = true;
}

int ShadowPages::swayLevel() const {
    const int l = static_cast<int>(std::lround(std::log2(config_.swayTexel / config_.finestTexel)));
    return std::clamp(l, 0, config_.levels - 1);
}

void ShadowPages::addDynamic(const bromath::AABB3& box, bool sways) { dynamic_.push_back({box, sways}); }

void ShadowPages::markDynamic() {
    // A sway-level page is wanted for its overlay when it is needed or a
    // needed page finer than it lies under it (the finer levels read its).
    const int s = swayLevel();
    std::vector<uint8_t> under;
    if (sun_.on) {
        under.assign(SP_LEVEL_ENTRIES, 0);
        for (int level = 0; level < s; ++level)
            for (int k = 0; k < SP_LEVEL_ENTRIES; ++k) {
                const Entry& e = entries_[static_cast<size_t>(level) * SP_LEVEL_ENTRIES + k];
                if (!e.occupied || !neededNow(e)) continue;
                const int a = clipmapEntry(s, e.x >> (s - level), e.y >> (s - level));
                if (a >= 0) under[static_cast<size_t>(a - s * SP_LEVEL_ENTRIES)] = 1;
            }
    }
    for (const Dynamic& d : dynamic_)
        forPages(d.box, s, d.sways ? s : SP_MAX_LEVELS, [&](int i) {
            Entry& e = entries_[static_cast<size_t>(i)];
            const bool atSway = i >= s * SP_LEVEL_ENTRIES && i < (s + 1) * SP_LEVEL_ENTRIES;
            if (neededNow(e) || (atSway && under[static_cast<size_t>(i - s * SP_LEVEL_ENTRIES)])) e.dynamic = true;
        });
}

void ShadowPages::setSeen(const bromath::AABB3& box) { seen_ = box; }

void ShadowPages::markBase() {
    // The sun: the finest level whose kBasePages pages a side span where a
    // shadow can fall in view (a pixel that wants a coarser level, with none
    // drawn there, takes the finer).
    // What is seen and what casts, both across the light: where a shadow
    // can fall in view.
    if (sun_.on && !bromath::aisEmpty(seen_)) {
        double rect[4];
        planeRect(seen_, rect);
        rect[0] = std::max(rect[0], sun_.casterRect[0]);
        rect[1] = std::min(rect[1], sun_.casterRect[1]);
        rect[2] = std::max(rect[2], sun_.casterRect[2]);
        rect[3] = std::min(rect[3], sun_.casterRect[3]);
        if (rect[0] <= rect[1] && rect[2] <= rect[3]) {
            const double span = std::max(rect[1] - rect[0], rect[3] - rect[2]);
            int level = 0;
            while (level < config_.levels - 1 && span > kBasePages * pageSize(level)) ++level;
            forLevelRect(level, rect, [this](int i) {
                Entry& e = entries_[static_cast<size_t>(i)];
                if (e.occupied) e.base = serial_;
            });
        }
    }
    // A local map: its coarsest mip's one page.
    for (const Map& m : maps_)
        if (m.kind != SP_KIND_CLIPMAP)
            entries_[static_cast<size_t>(m.first + mipFirst(SP_LOCAL_MIPS - 1))].base = serial_;
}

// --- Requests ---

void ShadowPages::request(uint64_t serial, std::span<const uint32_t> bits) {
    const History& h = history_[serial % 8];
    if (h.serial != serial || serial == 0) return;
    newestRequest_ = std::max(newestRequest_, serial);
    const int clip = config_.levels * SP_LEVEL_ENTRIES;
    const size_t words = std::min(bits.size(), (entries_.size() + 31) / 32);
    for (size_t w = 0; w < words; ++w) {
        uint32_t word = bits[w];
        while (word) {
            const int bit = std::countr_zero(word);
            word &= word - 1;
            const int i = static_cast<int>(w * 32 + bit);
            if (i >= entryCount()) break;
            Entry& e = entries_[static_cast<size_t>(i)];
            if (i < clip) {
                if (!h.sun) continue;
                const int level = i / SP_LEVEL_ENTRIES, slot = i % SP_LEVEL_ENTRIES;
                const int64_t x = h.ox[level] + wrap(slot % SP_WINDOW - h.ox[level]);
                const int64_t y = h.oy[level] + wrap(slot / SP_WINDOW - h.oy[level]);
                if (!e.occupied || e.x != x || e.y != y) continue;   // gone from the window since
            } else {
                const int s = (i - clip) / SP_LOCAL_ENTRIES;
                if (!locals_[s].used || h.localId[s] != locals_[s].lightId || h.localFace[s] != locals_[s].face)
                    continue;
            }
            e.needed = std::max(e.needed, serial);
        }
    }
}

bool ShadowPages::neededNow(const Entry& e) const {
    return (e.needed != 0 && e.needed + 1 >= newestRequest_) || e.base == serial_;
}

}  // namespace bro::scene
