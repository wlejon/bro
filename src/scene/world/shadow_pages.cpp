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
    entries_.assign(static_cast<size_t>(clipFirst(true)) + static_cast<size_t>(config_.levels) * SP_LEVEL_ENTRIES,
                    Entry{});
    owner_.assign(static_cast<size_t>(total), -1);
    free_.clear();
    for (int p = total - 1; p >= 0; --p) free_.push_back(p);
    sun_ = Sun{};
    next_ = Sun{};
    building_ = ready_ = false;
    begun_ = shown_ = 0;
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

void ShadowPages::sunPlane(const Sun& sun, const double p[3], double& a, double& b, double& c) {
    const double q[3] = {p[0] - sun.anchor[0], p[1] - sun.anchor[1], p[2] - sun.anchor[2]};
    a = dot3(sun.s, q) + sun.offset[0];
    b = -dot3(sun.u, q) + sun.offset[1];
    c = dot3(sun.d, q) + sun.offset[2];
}

void ShadowPages::sunCoordinates(const bromath::Vec3& p, double& a, double& b, double& c, bool next) const {
    const double q[3] = {p.x, p.y, p.z};
    sunPlane(sunOf(next), q, a, b, c);
}

void ShadowPages::window(int level, int64_t& x, int64_t& y) const {
    x = sun_.ox[level];
    y = sun_.oy[level];
}

int ShadowPages::clipmapEntry(int level, int64_t x, int64_t y, bool next) const {
    if (level < 0 || level >= config_.levels) return -1;
    const Sun& sun = sunOf(next);
    if (x < sun.ox[level] || x >= sun.ox[level] + SP_WINDOW || y < sun.oy[level] || y >= sun.oy[level] + SP_WINDOW)
        return -1;
    return clipFirst(next) + level * SP_LEVEL_ENTRIES + static_cast<int>(wrap(y) * SP_WINDOW + wrap(x));
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
    // (The maps run in the lights' order: a local light listed before the
    // sun puts its faces before the sun's levels.)
    if (index < clip || isNext(index)) {
        const bool next = isNext(index);
        const int level = (index - clipFirst(next)) / SP_LEVEL_ENTRIES;
        for (size_t m = 0; m < maps_.size(); ++m)
            if (maps_[m].kind == SP_KIND_CLIPMAP && maps_[m].next == next && maps_[m].level == level)
                return static_cast<int>(m);
        return -1;
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

void ShadowPages::moveWindows(Sun& sun, int first) {
    const double eye[3] = {eye_.x, eye_.y, eye_.z};
    double ea, eb, ec;
    sunPlane(sun, eye, ea, eb, ec);
    for (int level = 0; level < config_.levels; ++level) {
        const double size = pageSize(level);
        const int64_t ox = pageOf(ea, size) - SP_WINDOW / 2;
        const int64_t oy = pageOf(eb, size) - SP_WINDOW / 2;
        if (sun.haveWindows && ox == sun.ox[level] && oy == sun.oy[level]) continue;
        sun.ox[level] = ox;
        sun.oy[level] = oy;
        // Each slot holds the one page of the window congruent to it: a page
        // the window left gives its slot (and its physical page) to the one
        // that came in.
        for (int sy = 0; sy < SP_WINDOW; ++sy)
            for (int sx = 0; sx < SP_WINDOW; ++sx) {
                const int64_t x = ox + wrap(sx - ox), y = oy + wrap(sy - oy);
                const int i = first + level * SP_LEVEL_ENTRIES + sy * SP_WINDOW + sx;
                Entry& e = entries_[static_cast<size_t>(i)];
                if (e.occupied && e.x == x && e.y == y) continue;
                release(i);
                e.occupied = true;
                e.x = x;
                e.y = y;
                e.needed = 0;
            }
    }
    sun.haveWindows = true;
}

void ShadowPages::beginBasis(Sun& sun, const double d[3], float angularRadius, const Sun* from) const {
    // mlookAt's: across = normalize(d x up), up' = across x d.
    const double up[3] = {0.0, std::fabs(d[1]) > 0.99 ? 0.0 : 1.0, std::fabs(d[1]) > 0.99 ? 1.0 : 0.0};
    double s[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
    const double sl = std::sqrt(dot3(s, s));
    for (double& v : s) v /= sl;
    const double u[3] = {s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0]};
    const double eye[3] = {eye_.x, eye_.y, eye_.z};
    double offset[3] = {0.0, 0.0, 0.0};
    if (from) sunPlane(*from, eye, offset[0], offset[1], offset[2]);
    std::memcpy(sun.s, s, sizeof(s));
    std::memcpy(sun.u, u, sizeof(u));
    std::memcpy(sun.d, d, sizeof(sun.d));
    // A caster h above its receiver blurs over the disc's width at the
    // light's distance to it, h / sin(elevation) (the sun at the horizon
    // kept off it).
    sun.penumbra = static_cast<float>(2.0 * angularRadius / std::max(-d[1], 0.02));
    std::memcpy(sun.anchor, eye, sizeof(eye));
    std::memcpy(sun.offset, offset, sizeof(offset));
}

bool ShadowPages::fitRange(Sun& sun, const bromath::AABB3& box, bool grow) const {
    // The depth range along the light: the casters' extent, grown in coarse
    // steps (a change redraws every page, so it moves rarely) and never
    // shrunk.
    sun.casterRect[0] = sun.casterRect[2] = 1e300;
    sun.casterRect[1] = sun.casterRect[3] = -1e300;
    bool grew = false;
    if (!bromath::aisEmpty(box)) {
        double cs[8][3];
        corners(box, cs);
        double lo = 1e300, hi = -1e300;
        for (auto& p : cs) {
            double a, b, c;
            sunPlane(sun, p, a, b, c);
            lo = std::min(lo, c);
            hi = std::max(hi, c);
            sun.casterRect[0] = std::min(sun.casterRect[0], a);
            sun.casterRect[1] = std::max(sun.casterRect[1], a);
            sun.casterRect[2] = std::min(sun.casterRect[2], b);
            sun.casterRect[3] = std::max(sun.casterRect[3], b);
        }
        if (!sun.haveRange || lo < sun.zLo || hi > sun.zHi) {
            grew = sun.haveRange;
            if (grew && !grow) return true;
            if (sun.haveRange) {
                lo = std::min(lo, sun.zLo);
                hi = std::max(hi, sun.zHi);
            }
            const double step = std::exp2(std::ceil(std::log2(std::max(16.0, (hi - lo) * 0.25))));
            sun.zLo = std::floor((lo - step * 0.5) / step) * step;
            sun.zHi = std::ceil((hi + step * 0.5) / step) * step;
            sun.haveRange = true;
        }
    }
    if (!sun.haveRange) {
        sun.zLo = -64.0;
        sun.zHi = 64.0;
    }
    return grew;
}

void ShadowPages::show() {
    // The entries trade places (each physical page follows its entry): the
    // built clipmap's at the shown one's, the shown one's freed.
    const int clip = config_.levels * SP_LEVEL_ENTRIES, first = clipFirst(true);
    for (int i = 0; i < clip; ++i) {
        std::swap(entries_[static_cast<size_t>(i)], entries_[static_cast<size_t>(first + i)]);
        for (const int j : {i, first + i}) {
            const Entry& e = entries_[static_cast<size_t>(j)];
            if (e.physical >= 0) owner_[static_cast<size_t>(e.physical)] = j;
        }
    }
    std::swap(sun_, next_);
    releaseMap(first, clip);
    next_ = Sun{};
    building_ = ready_ = false;
    ++shown_;
}

void ShadowPages::addLevelMaps(const Sun& sun, bool next) {
    // A level's map: an ortho window over its pages, relative to the eye.
    const double eye[3] = {eye_.x, eye_.y, eye_.z};
    double ea, eb, ec;
    sunPlane(sun, eye, ea, eb, ec);
    bromath::Mat4 rot = bromath::midentity();
    for (int c = 0; c < 3; ++c) {
        rot.at(0, c) = static_cast<float>(sun.s[c]);
        rot.at(1, c) = static_cast<float>(sun.u[c]);
        rot.at(2, c) = static_cast<float>(-sun.d[c]);
    }
    const float zNear = static_cast<float>(sun.zLo - ec), zFar = static_cast<float>(sun.zHi - ec);
    for (int level = 0; level < config_.levels; ++level) {
        const double size = pageSize(level), w = size * SP_WINDOW;
        const double a0 = static_cast<double>(sun.ox[level]) * size - ea;
        const double b0 = static_cast<double>(sun.oy[level]) * size - eb;
        Map m;
        m.kind = SP_KIND_CLIPMAP;
        m.level = level;
        m.first = clipFirst(next) + level * SP_LEVEL_ENTRIES;
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
        m.next = next;
        maps_.push_back(m);
    }
}

void ShadowPages::setSun(const ShadowSun& sun) {
    double d[3] = {sun.direction.x, sun.direction.y, sun.direction.z};
    const double len = std::sqrt(dot3(d, d));
    if (len < 1e-9) return;
    for (double& v : d) v /= len;
    const int clip = config_.levels * SP_LEVEL_ENTRIES;
    if (!sun_.had || sun.lightId != sun_.lightId) {
        // Another light's pages: nothing of them holds.
        releaseMap(0, clip);
        releaseMap(clipFirst(true), clip);
        sun_ = Sun{};
        next_ = Sun{};
        building_ = ready_ = false;
        beginBasis(sun_, d, sun.angularRadius, nullptr);
        sun_.had = true;
        sun_.lightId = sun.lightId;
    } else if (building_ && (ready_ || serial_ - begun_ >= static_cast<uint64_t>(config_.buildFrames))) {
        show();
    }
    for (Sun* s : {&sun_, &next_}) {
        s->lightIndex = sun.lightIndex;
        s->bias = sun.bias;
        s->normalBias = sun.normalBias;
        s->reach = sun.reach;
    }
    sun_.on = true;

    // The sun turned past the shown clipmap, or its casters outgrew the
    // depth range: the next one begins, about the eye, in light space of the
    // sun as it is now.
    const bool outgrown = fitRange(sun_, sun.casters, false);
    if (!building_ && (outgrown || dot3(sun_.d, d) < std::cos(static_cast<double>(config_.sunTurn)))) {
        next_ = Sun{};
        next_.had = true;
        next_.lightId = sun.lightId;
        next_.lightIndex = sun.lightIndex;
        next_.bias = sun.bias;
        next_.normalBias = sun.normalBias;
        next_.reach = sun.reach;
        beginBasis(next_, d, sun.angularRadius, &sun_);
        building_ = true;
        ready_ = false;
        begun_ = serial_;
    }
    if (building_ && fitRange(next_, sun.casters, true))
        for (int i = clipFirst(true); i < clipFirst(true) + clip; ++i)
            if (entries_[static_cast<size_t>(i)].drawn) entries_[static_cast<size_t>(i)].dirty = true;
    moveWindows(sun_, 0);
    if (building_) moveWindows(next_, clipFirst(true));
    addLevelMaps(sun_, false);
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
    // The clipmap built: its levels' maps after the sampled ones.
    if (sun_.on && building_) addLevelMaps(next_, true);
    // The casters' changes met before the maps were set.
    lightsSet_ = true;
    for (const bromath::AABB3& box : invalid_) invalidate(box);
    invalid_.clear();
    History& h = history_[serial_ % 8];
    h.serial = serial_;
    h.sun = sun_.on;
    h.building = sun_.on && building_;
    h.shown = shown_;
    std::memcpy(h.ox, sun_.ox, sizeof(h.ox));
    std::memcpy(h.oy, sun_.oy, sizeof(h.oy));
    std::memcpy(h.nx, next_.ox, sizeof(h.nx));
    std::memcpy(h.ny, next_.oy, sizeof(h.ny));
    for (int s = 0; s < kLocalSlots; ++s) {
        h.localId[s] = locals_[s].used ? locals_[s].lightId : 0u;
        h.localFace[s] = locals_[s].face;
    }
}

// --- Casters ---

void ShadowPages::planeRect(const Sun& sun, const bromath::AABB3& box, double rect[4]) {
    double cs[8][3];
    corners(box, cs);
    rect[0] = rect[2] = 1e300;
    rect[1] = rect[3] = -1e300;
    for (auto& p : cs) {
        double a, b, c;
        sunPlane(sun, p, a, b, c);
        rect[0] = std::min(rect[0], a);
        rect[1] = std::max(rect[1], a);
        rect[2] = std::min(rect[2], b);
        rect[3] = std::max(rect[3], b);
    }
}

template <class F>
void ShadowPages::forLevelRect(const Map& m, const double rect[4], F&& f) {
    const int level = m.level;
    const double size = pageSize(level), border = static_cast<double>(m.texel) * SP_BORDER;
    const Sun& sun = sunOf(m.next);
    const int64_t ox = sun.ox[level], oy = sun.oy[level];
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
void ShadowPages::forPages(const bromath::AABB3& box, int minLevel, int maxLevel, bool next, F&& f) {
    if (bromath::aisEmpty(box)) return;
    double cs[8][3];
    corners(box, cs);
    double rect[4], nextRect[4];
    if (sun_.on) planeRect(sun_, box, rect);
    if (sun_.on && building_) planeRect(next_, box, nextRect);
    for (const Map& m : maps_) {
        if (m.kind == SP_KIND_CLIPMAP) {
            if (m.next && !next) continue;
            if (m.level >= minLevel && m.level <= maxLevel) forLevelRect(m, m.next ? nextRect : rect, f);
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
    forPages(box, 0, SP_MAX_LEVELS, true, [this](int i) {
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

int ShadowPages::searchLevel() const {
    const int l = static_cast<int>(std::lround(std::log2(config_.stillPenumbra / config_.finestTexel)));
    return std::clamp(l, std::min(swayLevel() + 1, config_.levels - 1), config_.levels - 1);
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
        forPages(d.box, s, d.sways ? s : SP_MAX_LEVELS, false, [&](int i) {
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
    // can fall in view. The clipmap built has its own, drawn before it is
    // shown.
    for (const Map& m : maps_) {
        if (m.kind != SP_KIND_CLIPMAP || m.level != 0 || bromath::aisEmpty(seen_)) continue;
        const Sun& sun = sunOf(m.next);
        double rect[4];
        planeRect(sun, seen_, rect);
        rect[0] = std::max(rect[0], sun.casterRect[0]);
        rect[1] = std::min(rect[1], sun.casterRect[1]);
        rect[2] = std::max(rect[2], sun.casterRect[2]);
        rect[3] = std::min(rect[3], sun.casterRect[3]);
        if (rect[0] > rect[1] || rect[2] > rect[3]) continue;
        const double span = std::max(rect[1] - rect[0], rect[3] - rect[2]);
        int level = 0;
        while (level < config_.levels - 1 && span > kBasePages * pageSize(level)) ++level;
        // (Where even the coarsest level spans it in more than kBasePages
        // pages, as from high up, the land to the horizon: that many pages
        // of it about the level's window's middle. The whole window was more
        // than the pool holds, needed every frame and never drawn.)
        const double most = kBasePages * pageSize(level);
        const int64_t mid[2] = {sun.ox[level] + SP_WINDOW / 2, sun.oy[level] + SP_WINDOW / 2};
        for (int a = 0; a < 2; ++a) {
            double& lo = rect[2 * a];
            double& hi = rect[2 * a + 1];
            if (hi - lo <= most) continue;
            const double c = std::clamp(static_cast<double>(mid[a]) * pageSize(level), lo + 0.5 * most, hi - 0.5 * most);
            lo = c - 0.5 * most;
            hi = c + 0.5 * most;
        }
        // (A clipmap's level maps run in order from its level 0's.)
        forLevelRect((&m)[level], rect, [this](int i) {
            Entry& e = entries_[static_cast<size_t>(i)];
            if (e.occupied) e.base = serial_;
        });
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
    const int clip = config_.levels * SP_LEVEL_ENTRIES, nextFirst = clipFirst(true);
    const size_t words = std::min(bits.size(), (entries_.size() + 31) / 32);
    for (size_t w = 0; w < words; ++w) {
        uint32_t word = bits[w];
        while (word) {
            const int bit = std::countr_zero(word);
            word &= word - 1;
            int i = static_cast<int>(w * 32 + bit);
            if (i >= entryCount()) break;
            if (i < clip || i >= nextFirst) {
                // A clipmap's page: the shown one's while no other has been
                // shown since; the one built then, now built or shown.
                if (!h.sun) continue;
                const bool next = i >= nextFirst;
                const int64_t *hx = h.ox, *hy = h.oy;
                if (next) {
                    if (!h.building) continue;
                    hx = h.nx;
                    hy = h.ny;
                    if (h.shown + 1 == shown_) i -= nextFirst;
                    else if (h.shown != shown_ || !building_) continue;
                } else if (h.shown != shown_) {
                    continue;
                }
                const int k = next ? static_cast<int>(w * 32 + bit) - nextFirst : i;
                const int level = k / SP_LEVEL_ENTRIES, slot = k % SP_LEVEL_ENTRIES;
                const int64_t x = hx[level] + wrap(slot % SP_WINDOW - hx[level]);
                const int64_t y = hy[level] + wrap(slot / SP_WINDOW - hy[level]);
                const Entry& e = entries_[static_cast<size_t>(i)];
                if (!e.occupied || e.x != x || e.y != y) continue;   // gone from the window since
            } else {
                const int s = (i - clip) / SP_LOCAL_ENTRIES;
                if (!locals_[s].used || h.localId[s] != locals_[s].lightId || h.localFace[s] != locals_[s].face)
                    continue;
            }
            Entry& e = entries_[static_cast<size_t>(i)];
            e.needed = std::max(e.needed, serial);
        }
    }
}

bool ShadowPages::neededNow(const Entry& e) const {
    return (e.needed != 0 && e.needed + 1 >= newestRequest_) || e.base == serial_;
}

}  // namespace bro::scene
