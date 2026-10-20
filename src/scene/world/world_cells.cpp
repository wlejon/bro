#include "scene/world/world_cells.h"

#include "scene/vulkan/shaders/world/world_cells_layout.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace bro::scene {

namespace {

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

void unite(bromath::AABB3& into, const bromath::AABB3& b) {
    into.min = bromath::vmin(into.min, b.min);
    into.max = bromath::vmax(into.max, b.max);
}

void putCell(uint32_t* w, const bromath::AABB3& b, uint32_t first, uint32_t count, const CellKey& key) {
    const float v[] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
    for (int j = 0; j < 6; ++j) w[WC_CELL_MIN + j] = bitsOf(v[j]);
    w[WC_CELL_FIRST] = first;
    w[WC_CELL_COUNT] = count;
    const uint64_t k = key.packed();
    w[WC_CELL_KEY] = static_cast<uint32_t>(k);
    w[WC_CELL_KEY + 1] = static_cast<uint32_t>(k >> 32);
}

}  // namespace

bool CellGrid::before(const CellKey& a, const CellKey& b) const {
    if (a.root != b.root) return a.root < b.root;
    const CellKey ta = topOf(a), tb = topOf(b);
    if (ta.z != tb.z) return ta.z < tb.z;
    if (ta.x != tb.x) return ta.x < tb.x;
    if (a.z != b.z) return a.z < b.z;
    return a.x < b.x;
}

std::vector<CellContent> sortIntoCells(const CellGrid& grid, size_t count,
                                       const std::function<bromath::Vec2(size_t)>& at, std::vector<uint32_t>& order) {
    std::vector<CellKey> keys(count);
    for (size_t i = 0; i < count; ++i) {
        const bromath::Vec2 p = at(i);
        keys[i] = grid.leafAt(p.x, p.y);
    }
    order.resize(count);
    std::iota(order.begin(), order.end(), 0u);
    // (Stable: a leaf keeps its items in their own order.)
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return grid.before(keys[a], keys[b]); });
    std::vector<CellContent> cells;
    for (size_t k = 0; k < count; ++k) {
        const CellKey& key = keys[order[k]];
        if (cells.empty() || cells.back().key != key) {
            CellContent c;
            c.key = key;
            c.first = static_cast<uint32_t>(k);
            cells.push_back(c);
        }
        cells.back().count++;
    }
    return cells;
}

WorldCells::Provider WorldCells::addProvider() {
    for (size_t p = 0; p < providers_.size(); ++p)
        if (!providers_[p].live) {
            providers_[p] = Entry{};
            providers_[p].live = true;
            return static_cast<Provider>(p);
        }
    providers_.push_back(Entry{});
    providers_.back().live = true;
    return static_cast<Provider>(providers_.size() - 1);
}

void WorldCells::removeProvider(Provider p) {
    if (p >= providers_.size()) return;
    providers_[p] = Entry{};
    dirty_ = true;
}

void WorldCells::setContent(Provider p, std::vector<CellContent> content, uint64_t generation) {
    if (p >= providers_.size() || !providers_[p].live) return;
    for (CellContent& c : content)
        if (!c.hasOrigin) {
            grid_.originOf(c.key, c.origin);
            c.hasOrigin = true;
        }
    providers_[p].content = std::move(content);
    providers_[p].generation = generation;
    dirty_ = true;
}

const std::vector<CellContent>& WorldCells::content(Provider p) const {
    static const std::vector<CellContent> kNone;
    return p < providers_.size() ? providers_[p].content : kNone;
}

uint64_t WorldCells::generation(Provider p) const { return p < providers_.size() ? providers_[p].generation : 0; }

const std::vector<Cell>& WorldCells::leaves() const {
    rebuild();
    return leaves_;
}

const std::vector<Cell>& WorldCells::tops() const {
    rebuild();
    return tops_;
}

void WorldCells::rebuild() const {
    if (!dirty_) return;
    dirty_ = false;
    // Every provider's leaves merged by key, in quadtree order.
    std::vector<Cell> all;
    for (const Entry& e : providers_)
        for (const CellContent& c : e.content) {
            Cell cell;
            cell.key = c.key;
            cell.bounds = c.bounds;
            cell.generation = e.generation;
            grid_.originOf(c.key, cell.origin);
            all.push_back(cell);
        }
    std::stable_sort(all.begin(), all.end(), [&](const Cell& a, const Cell& b) { return grid_.before(a.key, b.key); });
    leaves_.clear();
    for (const Cell& c : all) {
        if (!leaves_.empty() && leaves_.back().key == c.key) {
            unite(leaves_.back().bounds, c.bounds);
            leaves_.back().generation = std::max(leaves_.back().generation, c.generation);
        } else {
            leaves_.push_back(c);
        }
    }
    tops_.clear();
    for (uint32_t i = 0; i < leaves_.size(); ++i) {
        const CellKey top = grid_.topOf(leaves_[i].key);
        if (tops_.empty() || tops_.back().key != top) {
            Cell t;
            t.key = top;
            grid_.originOf(top, t.origin);
            t.firstLeaf = i;
            tops_.push_back(t);
        }
        Cell& t = tops_.back();
        t.leaves++;
        unite(t.bounds, leaves_[i].bounds);
        t.generation = std::max(t.generation, leaves_[i].generation);
    }
}

void WorldCells::flatten(Provider p, std::vector<uint32_t>& words) const {
    const std::vector<CellContent>& leaves = content(p);
    // Its leaves' tops: runs of the leaves (in quadtree order, a top's
    // leaves together).
    struct Top {
        bromath::AABB3 bounds = bromath::aempty3();
        uint32_t first = 0, count = 0;
        CellKey key;
    };
    std::vector<Top> tops;
    for (uint32_t i = 0; i < leaves.size(); ++i) {
        const CellKey top = grid_.topOf(leaves[i].key);
        if (tops.empty() || top != tops.back().key) {
            tops.push_back(Top{});
            tops.back().first = i;
            tops.back().key = top;
        }
        tops.back().count++;
        unite(tops.back().bounds, leaves[i].bounds);
    }
    const uint32_t topAt = WC_H_WORDS;
    const uint32_t leafAt = topAt + static_cast<uint32_t>(tops.size()) * WC_CELL_WORDS;
    words.assign(leafAt + leaves.size() * WC_CELL_WORDS, 0u);
    words[WC_H_TOPS] = static_cast<uint32_t>(tops.size());
    words[WC_H_LEAVES] = static_cast<uint32_t>(leaves.size());
    words[WC_H_TOP_AT] = topAt;
    words[WC_H_LEAF_AT] = leafAt;
    for (size_t t = 0; t < tops.size(); ++t)
        putCell(words.data() + topAt + t * WC_CELL_WORDS, tops[t].bounds, tops[t].first, tops[t].count, tops[t].key);
    for (size_t l = 0; l < leaves.size(); ++l)
        putCell(words.data() + leafAt + l * WC_CELL_WORDS, leaves[l].bounds, leaves[l].first, leaves[l].count,
                leaves[l].key);
}

void WorldCells::eyeRelativeOrigins(Provider p, const double eye[3], float* out) const {
    const std::vector<CellContent>& leaves = content(p);
    for (size_t l = 0; l < leaves.size(); ++l) {
        float* o = out + l * WC_ORIGIN_WORDS;
        for (int j = 0; j < 3; ++j) o[j] = static_cast<float>(leaves[l].origin[j] - eye[j]);
        o[3] = 0.0f;
    }
}

}  // namespace bro::scene
