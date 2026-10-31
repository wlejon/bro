#include "tilecache/tile_tree.h"

#include <algorithm>
#include <cmath>

namespace bro::tilecache {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;

double length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3d unit(const Vec3d& v) {
    const double l = length(v);
    return l > 0 ? Vec3d{v.x / l, v.y / l, v.z / l} : v;
}

// Which child of its parent a tile is: child(i) of TileId.
uint32_t quadrant(const TileId& t) { return (t.x & 1u) + 2 * (t.y & 1u); }

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
    nodes_.clear();
    freeNodes_.clear();
    index_.clear();
    wanted_.clear();
    pruneCursor_ = 0;
    slotNode_.assign(config_.capacity, -1);
    freeSlots_.clear();
    for (uint32_t s = config_.capacity; s-- > 0;) freeSlots_.push_back(s);   // slot 0 handed out first
    // The roots: nodes 0 to rootCount - 1, never pruned.
    for (uint32_t r = 0; r < config_.domain.rootCount(); ++r) makeNode({r, 0, 0, 0}, -1);
}

void TileTree::footprintOf(Node& n) const {
    n.center = config_.domain.point(n.id, 0.5, 0.5);
    n.normal = config_.domain.kind == Domain::Kind::Plane ? Vec3d{0, 1, 0}
                                                          : unit(sub(n.center, config_.domain.center));
    static constexpr double kSamples[8][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.5, 0}, {0.5, 1}, {0, 0.5}, {1, 0.5}};
    n.radius = 0;
    n.sag = 0;
    for (const auto& s : kSamples) {
        const Vec3d d = sub(config_.domain.point(n.id, s[0], s[1]), n.center);
        n.radius = std::max(n.radius, length(d));
        n.sag = std::max(n.sag, -dot(d, n.normal));
    }
    n.cell = config_.domain.tileExtent(n.id) / std::max<uint32_t>(config_.tileSize, 1);
}

int32_t TileTree::find(const TileId& id) const {
    const auto it = index_.find(id.key());
    return it == index_.end() ? -1 : it->second;
}

int32_t TileTree::makeNode(const TileId& id, int32_t parent) {
    int32_t i;
    if (!freeNodes_.empty()) {
        i = freeNodes_.back();
        freeNodes_.pop_back();
        nodes_[i] = Node{};
    } else {
        i = static_cast<int32_t>(nodes_.size());
        nodes_.emplace_back();
    }
    Node& n = nodes_[i];
    n.id = id;
    n.parent = parent;
    n.live = true;
    footprintOf(n);
    resolve(n);
    index_.emplace(id.key(), i);
    return i;
}

int32_t TileTree::ensureChild(int32_t parent, uint32_t i) {
    if (const int32_t c = nodes_[parent].child[i]; c >= 0) return c;
    const int32_t c = makeNode(nodes_[parent].id.child(i), parent);   // may move nodes_
    nodes_[parent].child[i] = c;
    return c;
}

int32_t TileTree::ensure(const TileId& id) {
    if (const int32_t i = find(id); i >= 0) return i;
    return ensureChild(ensure(id.parent()), quadrant(id));   // roots are always there
}

void TileTree::resolve(Node& n) const {
    n.range = n.hasOwn ? n.own
            : n.parent >= 0 ? nodes_[n.parent].range
                            : HeightRange{config_.minHeight, config_.maxHeight};
}

HeightRange TileTree::range(const TileId& id) const {
    TileId t = id;
    int32_t i = find(t);
    while (i < 0 && t.level > 0) {
        t = t.parent();
        i = find(t);
    }
    for (; i >= 0; i = nodes_[i].parent)
        if (nodes_[i].hasOwn) return nodes_[i].own;
    return {config_.minHeight, config_.maxHeight};
}

void TileTree::setRange(const TileId& id, HeightRange r) {
    if (!(r.lo <= r.hi)) return;   // NaN or inverted: not a range
    Node& n = nodes_[ensure(id)];
    n.own = r;
    n.hasOwn = true;
    n.range = r;   // its descendants take it up as the next select reaches them
}

void TileTree::prune() {
    // A moving view leaves behind tiles nothing needs any more: a slice of
    // the nodes is swept each select once the tree holds a few capacities,
    // so no one select pays for all of them. A childless node that is not
    // wanted and holds no slot goes (and its range with it); its parent may
    // go on a later sweep.
    if (index_.size() <= 4 * static_cast<size_t>(config_.capacity)) return;
    const size_t total = nodes_.size();
    for (size_t k = 0; k < std::min<size_t>(total, 2048); ++k) {
        if (pruneCursor_ >= total) pruneCursor_ = 0;
        const int32_t i = static_cast<int32_t>(pruneCursor_++);
        Node& n = nodes_[i];
        if (!n.live || n.parent < 0 || wantedNow(n) || n.slot >= 0) continue;
        if (n.child[0] >= 0 || n.child[1] >= 0 || n.child[2] >= 0 || n.child[3] >= 0) continue;
        nodes_[n.parent].child[quadrant(n.id)] = -1;
        index_.erase(n.id.key());
        n.live = false;
        freeNodes_.push_back(i);
    }
}

Bounds TileTree::bounds(const Node& n, const HeightRange& r) const {
    const double h = std::max(std::abs(r.lo), std::abs(r.hi));
    // A plane's surface leaves its base square to it; a sphere's along each
    // point's own normal, which a sphere about the base center bounds at
    // radius + |h|.
    if (config_.domain.kind == Domain::Kind::Plane) return {n.center, std::sqrt(n.radius * n.radius + h * h)};
    return {n.center, n.radius + h};
}

Bounds TileTree::bounds(const TileId& id) const {
    if (const int32_t i = find(id); i >= 0) return bounds(nodes_[i], range(id));
    Node n;
    n.id = id;
    footprintOf(n);
    return bounds(n, range(id));
}

Bounds TileTree::tightBounds(const Node& n) const {
    const HeightRange& r = n.range;
    const double mid = 0.5 * (r.lo + r.hi), half = 0.5 * (r.hi - r.lo);
    const Vec3d c = {n.center.x + n.normal.x * mid, n.center.y + n.normal.y * mid, n.center.z + n.normal.z * mid};
    if (config_.domain.kind == Domain::Kind::Plane) return {c, std::sqrt(n.radius * n.radius + half * half)};
    // A base point b at height h sits at b + u_b h, u_b its own normal:
    // |b + u_b h - (c + n mid)| <= |b - c| + |h - mid| + |mid| |u_b - n|, and
    // |u_b - n| = |b - c| / radius on the sphere.
    return {c, n.radius * (1 + std::abs(mid) / config_.domain.radius) + half};
}

TileTree::Cull TileTree::cullFor(const View& view) const {
    Cull c;
    c.cx = 1 / std::sqrt(1 + view.tanHalfX * view.tanHalfX);
    c.cy = 1 / std::sqrt(1 + view.tanHalfY * view.tanHalfY);
    if (view.frustum && config_.domain.kind == Domain::Kind::CubeSphere) {
        c.r0 = std::max(config_.domain.radius + config_.minHeight, 0.0);
        const double d = length(sub(view.position, config_.domain.center));
        if (d > c.r0) {
            c.horizon = true;
            c.eyeReach = std::sqrt(d * d - c.r0 * c.r0);
        }
    }
    return c;
}

bool TileTree::visible(const Bounds& b, double top, const View& view, const Cull& cull) const {
    const Vec3d rel = sub(b.center, view.position);
    if (view.frustum) {
        const double z = dot(rel, view.forward);
        if (z < -b.radius) return false;
        // Each side plane through the eye, its normal's tangent-space form
        // scaled to unit length: a sphere beyond any of them is outside.
        const double x = std::abs(dot(rel, view.right)), y = std::abs(dot(rel, view.up));
        if ((x - z * view.tanHalfX) * cull.cx > b.radius) return false;
        if ((y - z * view.tanHalfY) * cull.cy > b.radius) return false;
    }
    if (cull.horizon) {
        // Beyond the horizon of the lowest surface (radius + minHeight): a
        // point h above it is seen no farther than the two tangent lengths
        // sqrt(d^2 - r0^2) + sqrt((r0 + h)^2 - r0^2), and no point of the tile
        // is more than its range's top above it.
        const double h = std::max(top - config_.minHeight, 0.0);
        const double reach = cull.eyeReach + std::sqrt(h * (2 * cull.r0 + h));
        if (length(rel) - b.radius > reach) return false;
    }
    return true;
}

double TileTree::error(const Node& n, const View& view, const Cull& cull, bool* seen) const {
    const Bounds b = tightBounds(n);
    *seen = visible(b, n.range.hi, view, cull);
    if (!*seen) return 0;
    // Two lower bounds on the distance to the tile, the larger kept: to its
    // bounding sphere, and to its slab (the base footprint's disc, extruded
    // over its height range along the base normal, the bottom lowered by the
    // base's own curvature). The sphere alone reads a camera within the
    // range's height of the base as touching every tile that near.
    const Vec3d rel = sub(view.position, n.center);
    const Vec3d& nn = n.normal;
    const double up = dot(rel, nn);
    const double across = length(sub(rel, {nn.x * up, nn.y * up, nn.z * up}));
    const double dh = std::max(across - n.radius, 0.0);
    const double bottom = n.range.lo - n.sag;
    const double dv = up > n.range.hi ? up - n.range.hi : (up < bottom ? bottom - up : 0.0);
    const double sphere = length(sub(view.position, b.center)) - b.radius;
    const double dist = std::max({sphere, std::sqrt(dh * dh + dv * dv), n.cell * 1e-3});
    return n.cell / dist * view.projScale / std::max(config_.pixelsPerCell, 1e-9);
}

uint32_t TileTree::select(const View& view) {
    ++frame_;
    eye_ = view.position;
    wanted_.clear();
    open_.clear();
    // Split the largest-error tile first, while it projects too large and the
    // wanted tree (every tile in it holds a slot) still fits. A tile is
    // reached after its parent, so its range resolves from the parent's.
    // Only a tile that would split waits in the heap for its turn; one that
    // would not is a leaf whatever the order.
    const Cull cull = cullFor(view);
    auto cmp = [](const Open& a, const Open& b) { return a.error != b.error ? a.error < b.error : a.key > b.key; };
    auto offer = [&](int32_t i) {
        Node& n = nodes_[i];
        resolve(n);
        n.error = error(n, view, cull, &n.visible);
        n.leaf = true;
        n.lastWanted = frame_;
        wanted_.push_back(i);
        if (n.error > 1.0 && n.id.level < config_.maxLevel) {
            open_.push_back({n.error, n.id.key(), i});
            std::push_heap(open_.begin(), open_.end(), cmp);
        }
    };
    const uint32_t roots = config_.domain.rootCount();
    for (uint32_t r = 0; r < roots; ++r) offer(static_cast<int32_t>(r));
    uint32_t count = roots;
    while (!open_.empty() && count + 4 <= config_.capacity) {
        std::pop_heap(open_.begin(), open_.end(), cmp);
        const int32_t i = open_.back().node;
        open_.pop_back();
        nodes_[i].leaf = false;
        count += 4;
        for (uint32_t k = 0; k < 4; ++k) offer(ensureChild(i, k));
    }
    // Out of date and unwanted: evicted. One being produced again keeps its
    // slot until it lands, and goes on a later select if still unwanted.
    uint32_t dropped = 0;
    if (generation_ > 0) {
        for (const int32_t v : slotNode_) {
            if (v < 0) continue;
            Node& n = nodes_[v];
            if (stale(n) && !n.again && !wantedNow(n)) {
                evict(n);
                ++stats_.evicted;
                ++dropped;
            }
        }
    }
    prune();
    return dropped;
}

int32_t TileTree::slotOf(const TileId& id) const {
    const int32_t i = find(id);
    return i >= 0 && nodes_[i].state == State::Ready ? nodes_[i].slot : -1;
}

std::vector<Production> TileTree::readyTiles() const {
    std::vector<Production> out;
    for (const int32_t i : slotNode_) {
        if (i < 0 || nodes_[i].state != State::Ready) continue;
        const Node& n = nodes_[i];
        out.push_back({n.id, static_cast<uint32_t>(n.slot), -1});
    }
    return out;
}

void TileTree::evict(Node& n) {
    freeSlots_.push_back(static_cast<uint32_t>(n.slot));
    slotNode_[n.slot] = -1;
    n.slot = -1;
    n.state = State::Missing;
    n.again = false;
}

void TileTree::refresh() { ++generation_; }

uint32_t TileTree::stale() const {
    uint32_t n = 0;
    for (const int32_t i : wanted_)
        if (stale(nodes_[i])) ++n;
    return n;
}

bool TileTree::before(const Node& p) const {
    // A batch is produced at once (one dispatch over all of it), so a child
    // cannot read a parent beside it; an earlier batch runs first.
    if (p.state == State::Producing || p.again) return p.batch != batch_ + 1;
    return p.state == State::Ready && p.gen == generation_;
}

std::vector<Production> TileTree::takeProductions(uint32_t n) {
    std::vector<int32_t> missing;
    for (const int32_t i : wanted_)
        if (nodes_[i].state == State::Missing) missing.push_back(i);
    // The largest projected error first: what is drawn most wrong, the
    // ground nearest the eye, refines before ground far off that a coarse
    // tile already draws nearly right. Siblings go together, ranked by their
    // parent's error: a parent draws in its children's place until all four
    // are ready, so one sibling left for last (culled, or far) would keep
    // the whole quad, and everything refined under it, at the parent. A
    // parent projects larger than its children, so along any chain the
    // parent still comes first (a child whose parent is not ready waits,
    // below).
    auto rank = [&](const Node& m) { return m.parent >= 0 ? nodes_[m.parent].error : m.error; };
    auto group = [&](const Node& m) { return m.parent >= 0 ? nodes_[m.parent].id.key() : m.id.key(); };
    std::sort(missing.begin(), missing.end(), [&](int32_t ia, int32_t ib) {
        const Node &a = nodes_[ia], &b = nodes_[ib];
        const double ra = rank(a), rb = rank(b);
        if (ra != rb) return ra > rb;
        if (a.id.level != b.id.level) return a.id.level < b.id.level;
        if (group(a) != group(b)) return group(a) < group(b);
        return a.id.key() < b.id.key();
    });
    // With no free slot, the ready tiles no view wants now give theirs: the
    // finest first, then the least recently wanted. Coarse tiles are few (a
    // level holds a quarter of the next), wanted again first (a turn, a
    // horizon's edge), and dearest to produce again (a cell of one stands
    // for the most ground), so keeping them costs little and losing them
    // brings a frame's worth of production back for one tile. Chosen once
    // for the batch.
    std::vector<int32_t> victims;
    size_t nextVictim = 0;
    bool victimsChosen = false;
    std::vector<Production> out;
    for (const int32_t i : missing) {
        if (out.size() >= n) break;
        Node& m = nodes_[i];
        int32_t parentSlot = -1;
        if (m.parent >= 0) {
            // Ready and up to date, or taken by an earlier batch.
            const Node& p = nodes_[m.parent];
            if (p.slot < 0 || !before(p)) continue;
            parentSlot = p.slot;
        }
        if (freeSlots_.empty()) {
            if (!victimsChosen) {
                victimsChosen = true;
                for (const int32_t v : slotNode_)
                    if (v >= 0 && nodes_[v].state == State::Ready && !nodes_[v].again && !wantedNow(nodes_[v]))
                        victims.push_back(v);
                const size_t k = std::min<size_t>(victims.size(), n);
                std::partial_sort(victims.begin(), victims.begin() + k, victims.end(), [&](int32_t ia, int32_t ib) {
                    const Node &a = nodes_[ia], &b = nodes_[ib];
                    if (a.id.level != b.id.level) return a.id.level > b.id.level;
                    if (a.lastWanted != b.lastWanted) return a.lastWanted < b.lastWanted;
                    return a.id.key() < b.id.key();
                });
                victims.resize(k);
            }
            if (nextVictim >= victims.size()) break;
            evict(nodes_[victims[nextVictim++]]);
            ++stats_.evicted;
        }
        m.slot = static_cast<int32_t>(freeSlots_.back());
        freeSlots_.pop_back();
        slotNode_[m.slot] = i;
        m.state = State::Producing;
        m.batch = batch_ + 1;
        m.takenGen = generation_;
        out.push_back({m.id, static_cast<uint32_t>(m.slot), parentSlot});
    }
    // Then the wanted tiles out of date, into their own slots, ranked as the
    // missing are: they draw, only from cells made for what has changed.
    if (out.size() < n && generation_ > 0) {
        std::vector<int32_t> stale;
        for (const int32_t i : wanted_)
            if (this->stale(nodes_[i]) && !nodes_[i].again) stale.push_back(i);
        std::sort(stale.begin(), stale.end(), [&](int32_t ia, int32_t ib) {
            const Node &a = nodes_[ia], &b = nodes_[ib];
            const double ra = rank(a), rb = rank(b);
            if (ra != rb) return ra > rb;
            if (a.id.level != b.id.level) return a.id.level < b.id.level;
            return a.id.key() < b.id.key();
        });
        for (const int32_t i : stale) {
            if (out.size() >= n) break;
            Node& m = nodes_[i];
            int32_t parentSlot = -1;
            if (m.parent >= 0) {
                const Node& p = nodes_[m.parent];
                if (p.slot < 0 || !before(p)) continue;
                parentSlot = p.slot;
            }
            m.again = true;
            m.batch = batch_ + 1;
            m.takenGen = generation_;
            out.push_back({m.id, static_cast<uint32_t>(m.slot), parentSlot});
        }
    }
    if (!out.empty()) ++batch_;
    return out;
}

void TileTree::markReady(const TileId& id) {
    const int32_t i = find(id);
    if (i < 0) return;
    Node& m = nodes_[i];
    if (m.state == State::Producing) {
        m.state = State::Ready;
    } else if (m.state == State::Ready && m.again) {
        m.again = false;
    } else {
        return;
    }
    m.gen = m.takenGen;
    ++stats_.produced;
}

void TileTree::abandon(const TileId& id) {
    const int32_t i = find(id);
    if (i < 0) return;
    if (nodes_[i].again) {
        nodes_[i].again = false;   // still ready, its old cells still out of date
    } else if (nodes_[i].state == State::Producing) {
        evict(nodes_[i]);
    } else {
        return;
    }
    // A child taken after it was produced from what it never got.
    for (const int32_t c : nodes_[i].child)
        if (c >= 0 && (nodes_[c].state == State::Producing || nodes_[c].again)) abandon(nodes_[c].id);
}

uint32_t TileTree::pending() const {
    uint32_t n = 0;
    for (const int32_t i : wanted_)
        if (nodes_[i].state != State::Ready || stale(nodes_[i])) ++n;
    return n;
}

void TileTree::coverInto(int32_t node, std::vector<DrawTile>& out, bool& ok) const {
    const Node& n = nodes_[node];
    const bool wanted = wantedNow(n), ready = n.state == State::Ready;
    auto self = [&] {
        if (wanted && !n.visible) return;   // covered, but culled
        DrawTile d;
        d.id = n.id;
        d.slot = static_cast<uint32_t>(n.slot);
        d.parentSlot = n.parent >= 0 && nodes_[n.parent].state == State::Ready ? nodes_[n.parent].slot : -1;
        d.bounds = bounds(n, n.range);
        out.push_back(d);
    };
    if (!wanted || n.leaf) {
        ok = ready;
        if (ready) self();
        return;
    }
    const size_t mark = out.size();
    bool all = true;
    for (uint32_t i = 0; i < 4 && all; ++i) {
        if (n.child[i] < 0) all = false;
        else coverInto(n.child[i], out, all);
    }
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
    out.reserve(wanted_.size());
    for (uint32_t r = 0; r < config_.domain.rootCount(); ++r) {
        bool ok = true;
        coverInto(static_cast<int32_t>(r), out, ok);
    }
    std::vector<std::pair<double, uint32_t>> order(out.size());
    for (uint32_t i = 0; i < out.size(); ++i) {
        const Bounds& b = out[i].bounds;
        const double dx = b.center.x - eye_.x, dy = b.center.y - eye_.y, dz = b.center.z - eye_.z;
        order[i] = {std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius, i};
    }
    std::sort(order.begin(), order.end());
    std::vector<DrawTile> sorted;
    sorted.reserve(out.size());
    for (const auto& o : order) sorted.push_back(out[o.second]);
    return sorted;
}

} // namespace bro::tilecache
