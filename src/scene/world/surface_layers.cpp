#include "scene/world/surface_layers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_set>

namespace bro::scene {

namespace {

constexpr uint64_t kFree = ~uint64_t{0};
// The ground's slope is its cells' heights over their width; bumps finer
// than a cell slope at least this much.
constexpr float kSlopeSine = 0.05f;

float boxDistance(const bromath::Vec3& p, const bromath::AABB3& b) {
    const float dx = std::max({b.min.x - p.x, 0.0f, p.x - b.max.x});
    const float dy = std::max({b.min.y - p.y, 0.0f, p.y - b.max.y});
    const float dz = std::max({b.min.z - p.z, 0.0f, p.z - b.max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/// The cells of side `cs` (the first at `origin` cells, `n` of them) the span
/// [a, b] overlaps (touching an edge is not overlapping; a span of no width
/// is in the cell it lies in); empty (lo > hi) when it misses them all.
void cellRange(double a, double b, double cs, int origin, int n, int& lo, int& hi) {
    const int first = static_cast<int>(std::floor(a / cs));
    const int last = std::max(first, static_cast<int>(std::ceil(b / cs)) - 1);
    lo = std::max(0, first - origin);
    hi = std::min(n - 1, last - origin);
}

}  // namespace

void SurfaceLayers::configure(const SurfaceLayerConfig& config, const bromath::AABB3& bounds, const SurfaceMap* map) {
    config_ = config;
    map_ = map;
    config_.slots = std::clamp(config_.slots, 1, kMaxSlots);
    config_.tileTexels = std::max(config_.tileTexels, 4);
    config_.border = std::max(config_.border, 0);
    int shift = 0;
    while ((1 << shift) < std::max(config_.pageSize, 2)) ++shift;
    config_.pageSize = 1 << shift;
    pageShift_ = shift;
    bounds_ = bounds;

    // Enough levels that the coarsest one's tile spans the whole extent (so
    // at most 2 x 2 of them cover it).
    const float extent = std::max({bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z, config_.tileMetres});
    int levels = 1;
    while (levels < config_.maxLevels && config_.tileMetres * static_cast<float>(1u << (levels - 1)) < extent) ++levels;
    levels_ = bounds.min.x <= bounds.max.x ? levels : 0;

    tiles_.clear();
    slotTile_.assign(static_cast<size_t>(config_.slots), kFree);
    freeSlots_.clear();
    for (int s = config_.slots - 1; s >= 0; --s) freeSlots_.push_back(s);
    pageTile_.assign(static_cast<size_t>(roots()) * levels_ * config_.pageSize * config_.pageSize, kFree);
    fills_.clear();
    pageWrites_.clear();
    stats_ = {};
    gridDirty_ = true;
}

bool SurfaceLayers::setBounds(const bromath::AABB3& bounds) {
    const float extent = std::max({bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z, config_.tileMetres});
    int levels = 1;
    while (levels < config_.maxLevels && config_.tileMetres * static_cast<float>(1u << (levels - 1)) < extent) ++levels;
    if (!configured() || levels > levels_) {
        configure(config_, bounds, map_);
        return true;
    }
    // (The grid follows at the next update: anew only when the bounds move
    // its cells, else just the cells under items that changed.)
    bounds_ = bounds;
    return false;
}

float SurfaceLayers::texelMetres(int level) const {
    return config_.tileMetres * static_cast<float>(1u << level) / static_cast<float>(config_.tileTexels);
}

uint32_t SurfaceLayers::encodePage(const CellKey& key, int slot, int pageShift) {
    return static_cast<uint32_t>(slot + 1) | (static_cast<uint32_t>(key.x >> pageShift) & 1023u) << 12 |
           (static_cast<uint32_t>(key.z >> pageShift) & 1023u) << 22;
}

uint32_t SurfaceLayers::pageIndex(const CellKey& key) const {
    const int n = config_.pageSize;
    const int layer = key.root * levels_ + key.level;
    return static_cast<uint32_t>((layer * n + (key.z & (n - 1))) * n + (key.x & (n - 1)));
}

float SurfaceLayers::select(const View& view, std::vector<Need>& out) const {
    out.clear();
    const int top = levels_ - 1;
    const double topSide = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << top);
    // Tiles that want refining, the most too coarse first: how many times
    // its texel is larger than texelPixels of the view's pixels there.
    struct Open {
        float error;
        CellKey key;
        bool operator<(const Open& o) const { return error < o.error; }
    };
    std::priority_queue<Open> open;
    const auto visit = [&](const CellKey& k) {
        const double side = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << k.level);
        // The ground under the tile with its border: its heights and its
        // items' generations.
        const Ground g = under(k, side / config_.tileTexels * config_.border);
        if (g.lo > g.hi) return;   // no ground here
        const double x0 = k.x * side, z0 = k.z * side, x1 = (k.x + 1) * side, z1 = (k.z + 1) * side;
        SurfaceMap::Patch patch;
        if (map_) {
            patch = map_->patch(k.root, x0, z0, x1, z1, g.lo, g.hi, view.eye);
        } else {
            patch.box.min = {static_cast<float>(x0), g.lo, static_cast<float>(z0)};
            patch.box.max = {static_cast<float>(x1), g.hi, static_cast<float>(z1)};
            patch.above = std::fabs(view.eye.y - 0.5f * (g.lo + g.hi));
        }
        const bromath::AABB3& box = patch.box;
        // (The coarsest tiles are kept whatever the view sees.)
        const bool seen = !patch.hidden && bromath::fintersects(view.frustum, box);
        if (k.level != top && !seen) return;
        const float dist = boxDistance(view.eye, box);
        float metresPerPixel = view.orthoMetresPerPixel;
        if (view.perspective) {
            // The eye's height over the ground's middle there, and the
            // ground's slope (its heights over the cells' width): a slope
            // facing the view meets it the steeper.
            const float d = std::max(dist, 1e-3f);
            const float vert = patch.above;
            const float slope = (g.hi - g.lo) / std::max(g.span, 1e-3f);
            const float sine = std::min(1.0f, vert / d + std::max(slope, kSlopeSine));
            metresPerPixel = d / (std::max(view.focalPixels, 1.0f) * sine);
        }
        const float texelPixels = texelMetres(k.level) / std::max(metresPerPixel, 1e-9f);
        out.push_back({k, dist, mix64(generation_ ^ g.sum), texelPixels});
        if (k.level == 0 || !seen) return;
        const float error = texelMetres(k.level) / (config_.texelPixels * std::max(metresPerPixel, 1e-9f));
        if (error > 1.0f) open.push({error, k});
    };
    for (int r = 0; r < roots(); ++r)
        for (int z = static_cast<int>(std::floor(bounds_.min.z / topSide));
             z <= static_cast<int>(std::floor(bounds_.max.z / topSide)); ++z)
            for (int x = static_cast<int>(std::floor(bounds_.min.x / topSide));
                 x <= static_cast<int>(std::floor(bounds_.max.x / topSide)); ++x)
                visit({x, z, static_cast<uint8_t>(top), static_cast<uint8_t>(r)});
    // Refine the most too coarse while the atlas has room for its children:
    // what is left past it is drawn from its parents, as far from right as
    // the view allows.
    const size_t cap = static_cast<size_t>(config_.slots);
    while (!open.empty() && out.size() + 4 <= cap) {
        const CellKey k = open.top().key;
        open.pop();
        for (int c = 0; c < 4; ++c)
            visit({k.x * 2 + (c & 1), k.z * 2 + (c >> 1), static_cast<uint8_t>(k.level - 1), k.root});
    }
    return open.empty() ? 1.0f : open.top().error;
}

SurfaceLayers::GridLayout SurfaceLayers::gridLayout() const {
    GridLayout l;
    if (!configured()) return l;
    // Base cells a level-shift tile, starting where the coarsest tiles do
    // (so a level's cells are that level's tiles): kGridCells across the
    // coarsest tile where the budget allows, else as many as it does (a
    // power of two a coarsest tile, at least one).
    const int top = levels_ - 1;
    const double topSide = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << top);
    const int tx0 = static_cast<int>(std::floor(bounds_.min.x / topSide));
    const int tz0 = static_cast<int>(std::floor(bounds_.min.z / topSide));
    const int tx1 = static_cast<int>(std::floor(bounds_.max.x / topSide));
    const int tz1 = static_cast<int>(std::floor(bounds_.max.z / topSide));
    const size_t count = static_cast<size_t>(std::max(tx1 - tx0, tz1 - tz0) + 1);
    const size_t rootCount = static_cast<size_t>(std::max(roots(), 1));
    int cellsLog = 0;
    while (cellsLog < top && (2 << cellsLog) <= kGridCells) {
        const size_t n = count << (cellsLog + 1);
        if (n * n * rootCount > kGridBudget) break;
        ++cellsLog;
    }
    l.shift = top - cellsLog;
    l.x0 = tx0 << cellsLog;
    l.z0 = tz0 << cellsLog;
    l.n = static_cast<int>(count << cellsLog);
    return l;
}

bool SurfaceLayers::footprint(const SurfaceItem& item, int& i0, int& i1, int& j0, int& j1) const {
    const bromath::AABB3& b = item.bounds;
    if (b.min.x > b.max.x || item.root >= grids_.size()) return false;
    const double cs = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << gridShift_);
    cellRange(b.min.x, b.max.x, cs, gridX0_, gridN_, i0, i1);
    cellRange(b.min.z, b.max.z, cs, gridZ0_, gridN_, j0, j1);
    return i0 <= i1 && j0 <= j1;
}

void SurfaceLayers::link(uint32_t k, const SurfaceItem& item, std::vector<uint64_t>* dirty) {
    int i0, i1, j0, j1;
    if (!footprint(item, i0, i1, j0, j1)) return;
    std::vector<int32_t>& head = grids_[item.root].head;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t idx = static_cast<size_t>(j) * gridN_ + i;
            int32_t node = freeLink_;
            if (node >= 0) {
                freeLink_ = links_[static_cast<size_t>(node)].next;
            } else {
                node = static_cast<int32_t>(links_.size());
                links_.push_back({});
            }
            links_[static_cast<size_t>(node)] = {k, head[idx]};
            head[idx] = node;
            if (dirty) dirty->push_back(uint64_t{item.root} << 32 | idx);
        }
}

void SurfaceLayers::unlink(uint32_t k, const SurfaceItem& item, std::vector<uint64_t>& dirty) {
    int i0, i1, j0, j1;
    if (!footprint(item, i0, i1, j0, j1)) return;
    std::vector<int32_t>& head = grids_[item.root].head;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t idx = static_cast<size_t>(j) * gridN_ + i;
            for (int32_t* at = &head[idx]; *at >= 0; at = &links_[static_cast<size_t>(*at)].next) {
                const int32_t node = *at;
                if (links_[static_cast<size_t>(node)].item != k) continue;
                *at = links_[static_cast<size_t>(node)].next;
                links_[static_cast<size_t>(node)].next = freeLink_;
                freeLink_ = node;
                break;
            }
            dirty.push_back(uint64_t{item.root} << 32 | idx);
        }
}

SurfaceLayers::Cell SurfaceLayers::gather(int32_t node) const {
    Cell c{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0};
    for (; node >= 0; node = links_[static_cast<size_t>(node)].next) {
        const SurfaceItem& it = gridItems_[links_[static_cast<size_t>(node)].item];
        c.lo = std::min(c.lo, it.bounds.min.y);
        c.hi = std::max(c.hi, it.bounds.max.y);
        c.sum += mix64(it.generation);
    }
    return c;
}

void SurfaceLayers::buildGrid(const GridLayout& layout) {
    gridDirty_ = false;
    grids_.clear();
    links_.clear();
    freeLink_ = -1;
    gridShift_ = layout.shift;
    gridX0_ = layout.x0;
    gridZ0_ = layout.z0;
    gridN_ = layout.n;
    gridTouched_ = 0;
    if (!configured()) return;
    const int n = gridN_;
    const size_t cells = static_cast<size_t>(n) * n;
    const Cell empty{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0};
    grids_.resize(static_cast<size_t>(roots()));
    for (Grid& grid : grids_) {
        grid.cells.emplace_back(cells, empty);
        grid.side.push_back(n);
        grid.head.assign(cells, -1);
    }
    size_t total = 0;
    for (const SurfaceItem& item : gridItems_) {
        int i0, i1, j0, j1;
        if (footprint(item, i0, i1, j0, j1)) total += static_cast<size_t>(i1 - i0 + 1) * (j1 - j0 + 1);
    }
    links_.reserve(total);
    for (size_t k = 0; k < gridItems_.size(); ++k) link(static_cast<uint32_t>(k), gridItems_[k], nullptr);
    for (Grid& grid : grids_)
        for (size_t idx = 0; idx < cells; ++idx) grid.cells[0][idx] = gather(grid.head[idx]);
    gridTouched_ = static_cast<uint32_t>(cells * grids_.size());
    // Each coarser level's cell gathers its four.
    for (Grid& grid : grids_)
        while (grid.side.back() > 1) {
            const int s = grid.side.back(), h = (s + 1) / 2;
            std::vector<Cell> coarse(static_cast<size_t>(h) * h, empty);
            const std::vector<Cell>& fine = grid.cells.back();
            for (int j = 0; j < s; ++j)
                for (int x = 0; x < s; ++x) {
                    const Cell& f = fine[static_cast<size_t>(j) * s + x];
                    Cell& c = coarse[static_cast<size_t>(j / 2) * h + x / 2];
                    c.lo = std::min(c.lo, f.lo);
                    c.hi = std::max(c.hi, f.hi);
                    c.sum += f.sum;
                }
            grid.cells.push_back(std::move(coarse));
            grid.side.push_back(h);
        }
}

bool SurfaceLayers::updateGrid() {
    gridTouched_ = 0;
    const GridLayout layout = gridLayout();
    bool rebuild = gridDirty_ || !(layout == GridLayout{gridShift_, gridX0_, gridZ0_, gridN_}) ||
                         grids_.size() != static_cast<size_t>(roots());
    std::vector<SurfaceItem> next;
    if (itemsDirty_) {
        itemsDirty_ = false;
        next = std::move(items_);
        items_ = {};
        ownItems_ = !next.empty();
    } else if (ownItems_) {
        if (!rebuild) return false;
        next = std::move(gridItems_);
    }
    // (With no items, every root's chart is one item of generation 0.)
    if (!ownItems_) {
        next.clear();
        for (int r = 0; r < roots(); ++r) next.push_back({bounds_, 0, static_cast<uint8_t>(r)});
    }
    // The items told apart by index: a changed one leaves the cells under
    // its old box and joins those under its new one. When most changed (the
    // land's first boxes in place of its charts), building the grid again
    // is cheaper than relinking them, and their cells' sort.
    const auto same = [](const SurfaceItem& a, const SurfaceItem& b) {
        return a.root == b.root && a.generation == b.generation &&
               std::memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0;
    };
    const size_t common = std::min(gridItems_.size(), next.size());
    if (!rebuild) {
        size_t changed = std::max(gridItems_.size(), next.size()) - common;
        for (size_t k = 0; k < common; ++k) changed += !same(gridItems_[k], next[k]);
        if (changed * 4 > std::max(gridItems_.size(), next.size())) rebuild = true;
    }
    if (rebuild) {
        gridItems_ = std::move(next);
        buildGrid(layout);
        return true;
    }

    std::vector<uint64_t> dirty;
    for (size_t k = 0; k < common; ++k)
        if (!same(gridItems_[k], next[k])) {
            unlink(static_cast<uint32_t>(k), gridItems_[k], dirty);
            link(static_cast<uint32_t>(k), next[k], &dirty);
        }
    for (size_t k = common; k < gridItems_.size(); ++k) unlink(static_cast<uint32_t>(k), gridItems_[k], dirty);
    for (size_t k = common; k < next.size(); ++k) link(static_cast<uint32_t>(k), next[k], &dirty);
    gridItems_ = std::move(next);
    if (dirty.empty()) return false;
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    gridTouched_ = static_cast<uint32_t>(dirty.size());

    // Re-gather those cells, then their ancestors level by level.
    const Cell empty{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0};
    std::vector<uint32_t> at, up;
    for (size_t a = 0; a < dirty.size();) {
        const uint32_t root = static_cast<uint32_t>(dirty[a] >> 32);
        Grid& grid = grids_[root];
        at.clear();
        for (; a < dirty.size() && (dirty[a] >> 32) == root; ++a) {
            const uint32_t idx = static_cast<uint32_t>(dirty[a]);
            grid.cells[0][idx] = gather(grid.head[idx]);
            at.push_back(idx);
        }
        for (size_t p = 1; p < grid.cells.size(); ++p) {
            const int s = grid.side[p - 1], h = grid.side[p];
            up.clear();
            for (const uint32_t idx : at)
                up.push_back(static_cast<uint32_t>((idx / s / 2) * h + (idx % s) / 2));
            std::sort(up.begin(), up.end());
            up.erase(std::unique(up.begin(), up.end()), up.end());
            const std::vector<Cell>& fine = grid.cells[p - 1];
            for (const uint32_t idx : up) {
                const int px = static_cast<int>(idx % h), pz = static_cast<int>(idx / h);
                Cell c = empty;
                for (int dz = 0; dz < 2; ++dz)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = px * 2 + dx, z = pz * 2 + dz;
                        if (x >= s || z >= s) continue;
                        const Cell& f = fine[static_cast<size_t>(z) * s + x];
                        c.lo = std::min(c.lo, f.lo);
                        c.hi = std::max(c.hi, f.hi);
                        c.sum += f.sum;
                    }
                grid.cells[p][idx] = c;
            }
            at.swap(up);
        }
    }
    return true;
}

size_t SurfaceLayers::gridBytes() const {
    size_t bytes = links_.capacity() * sizeof(Link) + (gridItems_.capacity() + items_.capacity()) * sizeof(SurfaceItem);
    for (const Grid& grid : grids_) {
        bytes += grid.head.capacity() * sizeof(int32_t);
        for (const std::vector<Cell>& level : grid.cells) bytes += level.capacity() * sizeof(Cell);
    }
    return bytes;
}

SurfaceLayers::Ground SurfaceLayers::under(const CellKey& key, double pad) const {
    Ground out;
    out.lo = std::numeric_limits<float>::max();
    out.hi = -std::numeric_limits<float>::max();
    out.sum = 0;
    if (key.root >= grids_.size()) return out;
    const Grid& grid = grids_[key.root];
    // The level whose cells are this tile's size (the base for finer tiles).
    const int p = std::clamp(static_cast<int>(key.level) - gridShift_, 0, static_cast<int>(grid.cells.size()) - 1);
    const double side = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << key.level);
    const double cs = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << (gridShift_ + p));
    const int n = grid.side[static_cast<size_t>(p)];
    const int ox = gridX0_ >> p, oz = gridZ0_ >> p;
    const double x0 = key.x * side - pad, x1 = (key.x + 1) * side + pad;
    const double z0 = key.z * side - pad, z1 = (key.z + 1) * side + pad;
    int i0, i1, j0, j1;
    cellRange(x0, x1, cs, ox, n, i0, i1);
    cellRange(z0, z1, cs, oz, n, j0, j1);
    const std::vector<Cell>& g = grid.cells[static_cast<size_t>(p)];
    out.span = static_cast<float>(cs * std::max(1, std::min(i1 - i0, j1 - j0) + 1));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const Cell& c = g[static_cast<size_t>(j) * n + i];
            out.lo = std::min(out.lo, c.lo);
            out.hi = std::max(out.hi, c.hi);
            out.sum += c.sum;
        }
    return out;
}

int SurfaceLayers::takeSlot(uint64_t frame) {
    if (!freeSlots_.empty()) {
        const int s = freeSlots_.back();
        freeSlots_.pop_back();
        return s;
    }
    // The tile needed longest ago, never one this frame needs.
    int best = -1;
    uint64_t oldest = frame;
    for (int s = 0; s < config_.slots; ++s) {
        const auto it = tiles_.find(slotTile_[static_cast<size_t>(s)]);
        if (it == tiles_.end() || it->second.lastNeeded >= oldest) continue;
        oldest = it->second.lastNeeded;
        best = s;
    }
    if (best < 0) return -1;
    evict(slotTile_[static_cast<size_t>(best)]);
    freeSlots_.pop_back();   // evict returned it
    return best;
}

void SurfaceLayers::evict(uint64_t packed) {
    const auto it = tiles_.find(packed);
    if (it == tiles_.end()) return;
    const Tile& t = it->second;
    const uint32_t idx = pageIndex(t.key);
    if (pageTile_[idx] == packed) {
        pageTile_[idx] = kFree;
        const int n = config_.pageSize;
        pageWrites_.push_back({pageLayer(t.key), t.key.x & (n - 1), t.key.z & (n - 1), 0u});
    }
    if (t.slot >= 0) {
        slotTile_[static_cast<size_t>(t.slot)] = kFree;
        freeSlots_.push_back(t.slot);
    }
    tiles_.erase(it);
}

void SurfaceLayers::page(const Tile& tile) {
    const uint64_t packed = tile.key.packed();
    const uint32_t idx = pageIndex(tile.key);
    if (pageTile_[idx] == packed) return;
    // A tile a page away holds the entry: it goes (the nearer one was
    // chosen to fill).
    if (pageTile_[idx] != kFree) evict(pageTile_[idx]);
    pageTile_[idx] = packed;
    const int n = config_.pageSize;
    pageWrites_.push_back(
        {pageLayer(tile.key), tile.key.x & (n - 1), tile.key.z & (n - 1), encodePage(tile.key, tile.slot, pageShift_)});
}

void SurfaceLayers::update(const View& view, uint64_t frame) {
    jobs_.clear();
    fills_.clear();
    pageWrites_.clear();
    stats_ = {};
    frame_ = frame;
    if (!configured()) return;
    stats_.levels = static_cast<uint32_t>(levels_);
    const bool restaled = updateGrid();
    stats_.gridCells = gridTouched_;

    std::vector<Need> needs;
    stats_.worstError = select(view, needs);
    // Items changed: a held tile this frame does not need, made stale by
    // them, goes. (The draw reads the finest tile held wherever it wants
    // finer than the needed ones, so such a tile, kept as it was, showed the
    // ground as it was before: a region's heights standing as terraces
    // among the new where the eye had looked closer before.)
    if (restaled) {
        std::unordered_set<uint64_t> needed;
        needed.reserve(needs.size());
        for (const Need& n : needs) needed.insert(n.key.packed());
        std::vector<uint64_t> stale;
        for (const auto& [packed, t] : tiles_) {
            if (!t.filled || needed.count(packed)) continue;
            const double side = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << t.key.level);
            const Ground g = under(t.key, side / config_.tileTexels * config_.border);
            if (mix64(generation_ ^ g.sum) != t.generation) stale.push_back(packed);
        }
        for (const uint64_t packed : stale) evict(packed);
    }
    // Two needed tiles a page apart share an entry: the nearer is kept.
    {
        std::unordered_map<uint32_t, size_t> byEntry;
        std::vector<Need> kept;
        kept.reserve(needs.size());
        for (const Need& n : needs) {
            const auto [it, fresh] = byEntry.try_emplace(pageIndex(n.key), kept.size());
            if (fresh) {
                kept.push_back(n);
            } else if (n.distance < kept[it->second].distance) {
                kept[it->second] = n;
            }
        }
        needs.swap(kept);
    }
    stats_.needed = static_cast<uint32_t>(needs.size());

    // Coarsest first, so a tile's parent's error is known before its own.
    std::stable_sort(needs.begin(), needs.end(),
                     [](const Need& a, const Need& b) { return a.key.level > b.key.level; });
    const int top = levels_ - 1;
    // More coarsest tiles in view than the atlas could keep with their
    // children: the layer cannot hold even its base, so it offers nothing
    // and its fragments work everything out themselves. (Else they were
    // granted every frame and never stayed: a planet's turf, 2,400 of them
    // in 1,024 slots, ahead of every other producer's work.)
    size_t topNeeded = 0;
    for (const Need& n : needs) topNeeded += n.key.level == top;
    if (topNeeded > static_cast<size_t>(config_.slots / 2)) return;
    std::unordered_map<uint64_t, float> jobError;   // this frame's jobs, by key
    for (const Need& n : needs) {
        const uint64_t packed = n.key.packed();
        Tile& t = tiles_[packed];
        t.key = n.key;
        t.lastNeeded = frame;
        t.wanted = n.generation;
        if (t.filled && t.generation == n.generation) continue;
        // A stale tile shows its own texels; a missing one its nearest
        // resident ancestor's (none: as if one above the coarsest).
        int shown = n.key.level;
        if (!t.filled) {
            shown = levels_;
            for (int up = 1; n.key.level + up <= top; ++up) {
                const auto a = tiles_.find(n.key.parent(up).packed());
                if (a != tiles_.end() && a->second.filled) {
                    shown = n.key.level + up;
                    break;
                }
            }
        }
        float error = n.texelPixels * static_cast<float>(uint64_t{1} << (shown - n.key.level));
        if (n.key.level < top) {
            const auto p = jobError.find(n.key.parent().packed());
            if (p != jobError.end()) error = std::min(error, p->second);
        }
        jobError[packed] = error;
        const double side = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << n.key.level);
        jobs_.push_back({n.key, error, side, !t.filled && n.key.level == top});
    }
}

void SurfaceLayers::commit(std::span<const uint64_t> granted) {
    fills_.clear();
    if (!configured()) return;
    const uint64_t frame = frame_;
    for (const uint64_t packed : granted) {
        const auto it = tiles_.find(packed);
        if (it == tiles_.end() || it->second.lastNeeded != frame) continue;   // evicted as another's alias
        Tile& t = it->second;
        if (t.filled && t.generation == t.wanted) continue;
        if (t.slot < 0) {
            const int s = takeSlot(frame);
            if (s < 0) break;   // every slot is needed this frame
            t.slot = s;
            slotTile_[static_cast<size_t>(s)] = packed;
        }
        t.filled = true;
        t.generation = t.wanted;
        const double side = static_cast<double>(config_.tileMetres) * static_cast<double>(uint64_t{1} << t.key.level);
        fills_.push_back({t.key, t.slot, t.key.x * side, t.key.z * side, side});
        page(t);
    }
    stats_.filled = static_cast<uint32_t>(fills_.size());

    // Tiles needed this frame but given no slot are forgotten (they are
    // chosen again next frame); every other tile keeps its slot.
    stats_.missing = stats_.resident = 0;
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (it->second.slot < 0) {
            ++stats_.missing;
            it = tiles_.erase(it);
        } else {
            if (it->second.lastNeeded == frame && it->second.generation != it->second.wanted) ++stats_.missing;
            ++stats_.resident;
            ++it;
        }
    }
}

}  // namespace bro::scene
