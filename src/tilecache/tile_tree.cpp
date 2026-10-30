#include "tilecache/tile_tree.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace bro::tilecache {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;

double length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

} // namespace

// ---------------------------------------------------------------------------
// Domain

Domain::Frame Domain::frame(const TileId& t) const {
    const double n = std::ldexp(1.0, static_cast<int>(t.level));
    const double fu = t.x / n, fv = t.y / n;   // the tile's corner across its root, [0, 1]
    if (kind == Kind::Plane) {
        const uint32_t rx = t.root % rootsX, ry = t.root / rootsX;
        const double e = size / n;
        return {{origin[0] + (rx + fu) * size, 0.0, origin[1] + (ry + fv) * size}, {e, 0, 0}, {0, 0, e}};
    }
    // A face point at (s, q) in [-1, 1]^2 is origin + s * S + q * Q.
    Vec3d o, S, Q;
    switch (t.root) {
    case 0: o = {1, 0, 0}; S = {0, 0, -1}; Q = {0, 1, 0}; break;    // +X: (1, q, -s)
    case 1: o = {-1, 0, 0}; S = {0, 0, 1}; Q = {0, 1, 0}; break;    // -X: (-1, q, s)
    case 2: o = {0, 1, 0}; S = {1, 0, 0}; Q = {0, 0, -1}; break;    // +Y: (s, 1, -q)
    case 3: o = {0, -1, 0}; S = {1, 0, 0}; Q = {0, 0, 1}; break;    // -Y: (s, -1, q)
    case 4: o = {0, 0, 1}; S = {1, 0, 0}; Q = {0, 1, 0}; break;     // +Z: (s, q, 1)
    default: o = {0, 0, -1}; S = {-1, 0, 0}; Q = {0, 1, 0}; break;  // -Z: (-s, q, -1)
    }
    const double s0 = fu * 2 - 1, q0 = fv * 2 - 1, e = 2 / n;
    return {{o.x + s0 * S.x + q0 * Q.x, o.y + s0 * S.y + q0 * Q.y, o.z + s0 * S.z + q0 * Q.z},
            {S.x * e, S.y * e, S.z * e},
            {Q.x * e, Q.y * e, Q.z * e}};
}

Vec3d Domain::point(const TileId& t, double u, double v) const {
    const Frame f = frame(t);
    const Vec3d c = {f.corner.x + u * f.du.x + v * f.dv.x, f.corner.y + u * f.du.y + v * f.dv.y,
                     f.corner.z + u * f.du.z + v * f.dv.z};
    if (kind == Kind::Plane) return c;
    const double k = radius / length(c);
    return {center.x + c.x * k, center.y + c.y * k, center.z + c.z * k};
}

double Domain::tileExtent(const TileId& t) const {
    const double root = kind == Kind::Plane ? size : radius * kHalfPi;
    return std::ldexp(root, -static_cast<int>(t.level));
}

// ---------------------------------------------------------------------------
// TileTree

TileTree::TileTree(const Config& config) : config_(config) {
    config_.maxLevel = std::min(config_.maxLevel, TileId::kMaxLevel);
    config_.capacity = std::max(config_.capacity, config_.domain.rootCount());
    clear();
}

void TileTree::clear() {
    entries_.clear();
    wanted_.clear();
    freeSlots_.clear();
    for (uint32_t s = config_.capacity; s-- > 0;) freeSlots_.push_back(s);   // slot 0 handed out first
}

Bounds TileTree::bounds(const TileId& id) const {
    Bounds b;
    b.center = config_.domain.point(id, 0.5, 0.5);
    static constexpr double kSamples[8][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.5, 0}, {0.5, 1}, {0, 0.5}, {1, 0.5}};
    for (const auto& s : kSamples)
        b.radius = std::max(b.radius, length(sub(config_.domain.point(id, s[0], s[1]), b.center)));
    b.radius += config_.maxHeight;
    return b;
}

bool TileTree::visible(const Bounds& b, const View& view) const {
    const Vec3d rel = sub(b.center, view.position);
    if (view.frustum) {
        const double z = dot(rel, view.forward);
        if (z < -b.radius) return false;
        // Each side plane through the eye, its normal's tangent-space form
        // scaled to unit length: a sphere beyond any of them is outside.
        const double x = std::abs(dot(rel, view.right)), y = std::abs(dot(rel, view.up));
        const double cx = 1 / std::sqrt(1 + view.tanHalfX * view.tanHalfX);
        const double cy = 1 / std::sqrt(1 + view.tanHalfY * view.tanHalfY);
        if ((x - z * view.tanHalfX) * cx > b.radius) return false;
        if ((y - z * view.tanHalfY) * cy > b.radius) return false;
    }
    if (view.frustum && config_.domain.kind == Domain::Kind::CubeSphere) {
        // Beyond the horizon of the lowest surface (radius - maxHeight): a
        // point h above it is seen no farther than the two tangent lengths
        // sqrt(d^2 - r0^2) + sqrt((r0 + h)^2 - r0^2), and no point of the tile
        // is more than 2 maxHeight above it.
        const double r0 = std::max(config_.domain.radius - config_.maxHeight, 0.0);
        const double d = length(sub(view.position, config_.domain.center));
        if (d > r0) {
            const double h = 2 * config_.maxHeight;
            const double reach = std::sqrt(d * d - r0 * r0) + std::sqrt(h * (2 * r0 + h));
            if (length(rel) - b.radius > reach) return false;
        }
    }
    return true;
}

double TileTree::error(const TileId& id, const View& view, bool* seen) const {
    const Bounds b = bounds(id);
    *seen = visible(b, view);
    if (!*seen) return 0;
    const double cell = config_.domain.tileExtent(id) / std::max<uint32_t>(config_.tileSize, 1);
    const double dist = std::max(length(sub(view.position, b.center)) - b.radius, cell * 1e-3);
    return cell / dist * view.projScale / std::max(config_.pixelsPerCell, 1e-9);
}

void TileTree::select(const View& view) {
    ++frame_;
    wanted_.clear();
    // Split the largest-error tile first, while it projects too large and the
    // wanted tree (every tile in it holds a slot) still fits.
    auto cmp = [](const Wanted& a, const Wanted& b) {
        return a.error != b.error ? a.error < b.error : a.id.key() > b.id.key();
    };
    std::priority_queue<Wanted, std::vector<Wanted>, decltype(cmp)> open(cmp);
    auto offer = [&](const TileId& id) {
        Wanted w{id};
        w.error = error(id, view, &w.visible);
        open.push(w);
    };
    for (uint32_t r = 0; r < config_.domain.rootCount(); ++r) offer({r, 0, 0, 0});
    uint32_t count = config_.domain.rootCount();
    while (!open.empty()) {
        Wanted w = open.top();
        open.pop();
        if (w.error > 1.0 && w.id.level < config_.maxLevel && count + 4 <= config_.capacity) {
            w.leaf = false;
            count += 4;
            for (uint32_t i = 0; i < 4; ++i) offer(w.id.child(i));
        }
        wanted_[w.id.key()] = w;
    }
    for (const auto& [key, w] : wanted_) {
        Entry& e = entries_[key];
        e.id = w.id;
        e.lastWanted = frame_;
    }
    // Entries holding nothing that no view wants are dropped.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.slot < 0 && it->second.lastWanted != frame_) it = entries_.erase(it);
        else ++it;
    }
}

bool TileTree::isReady(const TileId& id) const {
    auto it = entries_.find(id.key());
    return it != entries_.end() && it->second.state == State::Ready;
}

int32_t TileTree::slotOf(const TileId& id) const {
    auto it = entries_.find(id.key());
    return it != entries_.end() && it->second.state == State::Ready ? it->second.slot : -1;
}

bool TileTree::allocate(Entry& e) {
    if (freeSlots_.empty()) {
        // The least recently wanted ready tile no view wants now; of equals,
        // the finest (its parent is likelier to be wanted again first).
        Entry* victim = nullptr;
        for (auto& [key, c] : entries_) {
            if (c.state != State::Ready || c.lastWanted == frame_) continue;
            if (!victim || c.lastWanted < victim->lastWanted ||
                (c.lastWanted == victim->lastWanted && c.id.level > victim->id.level))
                victim = &c;
        }
        if (!victim) return false;
        freeSlots_.push_back(static_cast<uint32_t>(victim->slot));
        victim->slot = -1;
        victim->state = State::Missing;
        ++stats_.evicted;
    }
    e.slot = static_cast<int32_t>(freeSlots_.back());
    freeSlots_.pop_back();
    return true;
}

std::vector<Production> TileTree::takeProductions(uint32_t n) {
    std::vector<const Wanted*> missing;
    for (const auto& [key, w] : wanted_) {
        const Entry& e = entries_.at(key);
        if (e.state == State::Missing) missing.push_back(&w);
    }
    // Coarse first (a parent is ready before its children are offered), then
    // the largest projected error.
    std::sort(missing.begin(), missing.end(), [](const Wanted* a, const Wanted* b) {
        if (a->id.level != b->id.level) return a->id.level < b->id.level;
        if (a->error != b->error) return a->error > b->error;
        return a->id.key() < b->id.key();
    });
    std::vector<Production> out;
    for (const Wanted* w : missing) {
        if (out.size() >= n) break;
        int32_t parentSlot = -1;
        if (w->id.level > 0) {
            auto pit = entries_.find(w->id.parent().key());
            if (pit == entries_.end() || pit->second.slot < 0 || pit->second.state == State::Missing) continue;
            parentSlot = pit->second.slot;   // ready, or producing earlier in this batch
        }
        Entry& e = entries_.at(w->id.key());
        if (!allocate(e)) break;
        e.state = State::Producing;
        out.push_back({w->id, static_cast<uint32_t>(e.slot), parentSlot});
    }
    return out;
}

void TileTree::markReady(const TileId& id) {
    auto it = entries_.find(id.key());
    if (it == entries_.end() || it->second.state != State::Producing) return;
    it->second.state = State::Ready;
    ++stats_.produced;
}

void TileTree::abandon(const TileId& id) {
    auto it = entries_.find(id.key());
    if (it == entries_.end() || it->second.state != State::Producing) return;
    freeSlots_.push_back(static_cast<uint32_t>(it->second.slot));
    it->second.slot = -1;
    it->second.state = State::Missing;
}

uint32_t TileTree::pending() const {
    uint32_t n = 0;
    for (const auto& [key, w] : wanted_)
        if (!isReady(w.id)) ++n;
    return n;
}

void TileTree::coverInto(const TileId& id, std::vector<DrawTile>& out, bool& ok) const {
    const auto wit = wanted_.find(id.key());
    const bool ready = isReady(id);
    auto self = [&] {
        if (wit != wanted_.end() && !wit->second.visible) return;   // covered, but culled
        DrawTile d;
        d.id = id;
        d.slot = static_cast<uint32_t>(slotOf(id));
        d.parentSlot = id.level > 0 ? slotOf(id.parent()) : -1;
        d.bounds = bounds(id);
        out.push_back(d);
    };
    if (wit == wanted_.end() || wit->second.leaf) {
        ok = ready;
        if (ready) self();
        return;
    }
    const size_t mark = out.size();
    bool all = true;
    for (uint32_t i = 0; i < 4 && all; ++i) coverInto(id.child(i), out, all);
    if (all) {
        ok = true;
        return;
    }
    out.resize(mark);
    ok = ready;
    if (ready) self();
}

std::vector<DrawTile> TileTree::drawSet() const {
    std::vector<DrawTile> out;
    for (uint32_t r = 0; r < config_.domain.rootCount(); ++r) {
        bool ok = true;
        coverInto({r, 0, 0, 0}, out, ok);
    }
    return out;
}

} // namespace bro::tilecache
