#include "bronze_host/host_tiles.h"

#include "bronze_host/host_kernel_gpu.h"
#include "bronze_host/host_rooted.h"
#include "tilecache/tile_tree.h"
#include "tilecache/tiles_module.h"
#include "engine/engine.h"

#include "embed/embed_host_class.h"
#include "embed/embed_object_builder.h"

#include <brokernel/api.h>

#include "render/vulkan_compute.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;
using tilecache::DrawTile;

// A drawList record, 16 words: int32 slot, parentSlot, root, level, x, y,
// coarser (DrawTile::coarser, 4 bits an edge: u = 0 in the low bits, then
// u = 1, v = 0, v = 1), 0, then float32 centerHi.xyz, radius, centerLo.xyz, 0 (the bounding
// sphere's center as hi + lo, so a shader can subtract a camera split the
// same way and get a camera-relative position to f64 accuracy).
constexpr uint32_t kRecordInts = 16;

// A batch table record, 36 words (docs/tiles-api.js): int32 slot,
// parentSlot (-1 for a root), root, level, x, y, quadrant x, quadrant y,
// then float32 anchor.xyz, extent, (frame centre - anchor).xyz, radius,
// u.xyz, 0, v.xyz, 0, dirHi.xyz, 0, dirLo.xyz, 0, (anchor's surface point -
// center).xyz, 0. Everything a kernel needs to place a cell in f32 relative
// to the tile: the anchor is the frame's centre rounded to f32, and every
// other float is small beside it or exact.
constexpr uint32_t kTableWords = 36;

// The f32 anchor of a tile and what places its cells about it.
struct TileAnchor {
    float base[3];      // the frame's centre in base coordinates, rounded
    float lost[3];      // the centre less base
    float dirHi[3];     // the base surface's normal at the anchor (cube-sphere: normalize(base)), rounded
    float dirLo[3];     // what the rounding lost
    float point[3];     // the anchor's surface point less the tile's center
};

TileAnchor anchorOf(const tilecache::Domain& d, const tilecache::Domain::Frame& f, const tilecache::Vec3d& center) {
    TileAnchor a{};
    const double c[3] = {f.corner.x + 0.5 * (f.du.x + f.dv.x), f.corner.y + 0.5 * (f.du.y + f.dv.y),
                         f.corner.z + 0.5 * (f.du.z + f.dv.z)};
    double base[3], n[3];
    for (int k = 0; k < 3; ++k) {
        a.base[k] = static_cast<float>(c[k]);
        base[k] = a.base[k];
        a.lost[k] = static_cast<float>(c[k] - base[k]);
    }
    double surface[3];
    if (d.kind == tilecache::Domain::Kind::CubeSphere) {
        const double l = std::sqrt(base[0] * base[0] + base[1] * base[1] + base[2] * base[2]);
        const double dc[3] = {d.center.x, d.center.y, d.center.z};
        for (int k = 0; k < 3; ++k) {
            n[k] = base[k] / l;
            surface[k] = dc[k] + n[k] * d.radius;
        }
    } else {
        n[0] = 0; n[1] = 1; n[2] = 0;
        for (int k = 0; k < 3; ++k) surface[k] = base[k];
    }
    const double ce[3] = {center.x, center.y, center.z};
    for (int k = 0; k < 3; ++k) {
        a.dirHi[k] = static_cast<float>(n[k]);
        a.dirLo[k] = static_cast<float>(n[k] - a.dirHi[k]);
        a.point[k] = static_cast<float>(surface[k] - ce[k]);
    }
    return a;
}

// The lookup (tilecache/tiles.bk): int4 records, the first [mask, 0, 0, 0],
// then mask + 1 entries, a ready tile at the first free one from
// tilecache::lookupHash(id) & mask on: [x, y, root | level << 8 |
// (parent's slot + 1) << 13, slot], the parent's slot -1 unless it is
// ready (so a read blending a tile with its parent, as mips, finds both in
// one probe); an empty one [0, 0, -1, -1]. Twice the capacity, a power of
// two, so a probe ends within a few entries.
uint32_t lookupEntries(uint32_t capacity) {
    uint32_t n = 4;
    while (n < 2 * capacity) n *= 2;
    return n;
}

struct Layer {
    std::string name;
    uint32_t cellBytes = 4;
    size_t tileBytes = 0;
    std::shared_ptr<brokernel::api::GpuBuffer> pool;
};

struct Cache;
// The live caches, for the engine's settle state (installTiles). Caches are
// made and read on the page's thread.
std::vector<Cache*> s_live;

struct Cache {
    explicit Cache(const tilecache::Config& c) : tree(c) { s_live.push_back(this); }
    ~Cache() { std::erase(s_live, this); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    tilecache::TileTree tree;
    bool destroyed = false;      // destroy(): its GPU memory is released
    uint32_t border = 0;
    uint32_t cells = 0;          // a tile's cells along an edge, borders included
    uint32_t producePerUpdate = 8;
    std::vector<Layer> layers;
    std::vector<std::string> invalidatedLayers; // selective layer subset for stale refreshes (empty = all layers)
    std::shared_ptr<brokernel::api::GpuBuffer> drawList;
    std::vector<DrawTile> drawn;
    std::vector<int32_t> drawRecords;   // what drawList holds (writeDrawList)
    std::shared_ptr<brokernel::api::GpuBuffer> lookup;
    // The update's batch on the GPU, producePerUpdate records, and its
    // KernelBuffer, made once.
    std::shared_ptr<brokernel::api::GpuBuffer> table;
    ev::Persistent tableValue;

    // `ranges`: two float32 a slot, the [lo, hi] its producer wrote at
    // 2 * slot; `detail`: one float32 a slot, the error its split would take
    // away. Each batch's are read back without waiting and given to the tree.
    std::shared_ptr<brokernel::api::GpuBuffer> rangePool;
    std::shared_ptr<brokernel::api::GpuBuffer> detailPool;
    struct Report {
        std::shared_ptr<render::ComputeReadback> ranges, detail;
        uint32_t firstSlot = 0;
        std::vector<tilecache::Production> tiles;
        bool ready() const { return (!ranges || ranges->ready()) && (!detail || detail->ready()); }
    };
    std::deque<Report> reports;

    // The next batch's most tiles (pace()): up to producePerUpdate.
    uint32_t batchSize = 1;
    // A produced tile's GPU ms, learned from its batches' spans: over every
    // level (-1 before a batch is timed) and by level (-1: none of it
    // timed yet), as a producer's cost often follows the level (a coarse
    // tile's cells each stand for more ground to average); its pending
    // tiles' ms, as of its last update; the ms of its share of the
    // refinement not spent yet, and what this update's batch may spend.
    double tileMs = -1.0;
    std::vector<double> levelMs;
    // A batch's span is an overhead (its submission's latency on the queue,
    // whatever the batch holds) and its tiles' costs: fitted as
    // ms = batchMs + tiles x a tile's, by least squares over the batches
    // timed (sums decayed toward the latest), so a batch of few tiles does
    // not charge the overhead to them. The budget pays the tiles alone.
    double batchMs = 0.0;
    double fitW = 0.0, fitN = 0.0, fitM = 0.0, fitNN = 0.0, fitNM = 0.0;
    double wantMs = 0.0;
    double credit = 0.0;
    double spendMs = std::numeric_limits<double>::infinity();
    // A level's cost, else the nearest level's timed (the finer first: a
    // descent reaches levels finer than any timed), else the mean.
    double costOf(uint32_t level) const {
        const int32_t n = static_cast<int32_t>(levelMs.size());
        for (int32_t d = 0; d < n + static_cast<int32_t>(level); ++d) {
            const int32_t finer = static_cast<int32_t>(level) + d, coarser = static_cast<int32_t>(level) - d;
            if (finer < n && levelMs[finer] > 0.0) return levelMs[finer];
            if (coarser >= 0 && coarser < n && levelMs[coarser] > 0.0) return levelMs[coarser];
        }
        return tileMs;
    }

    // Batches produced in the background stream (VulkanCompute::
    // beginBackground), oldest first: in the gap after their frame, so no
    // frame waits for them or shares the GPU with them; their tiles are
    // ready once the GPU has done them.
    struct InFlight {
        std::vector<tilecache::Production> tiles;
        std::vector<bool> layerMask; // empty = all layers; else per-layer dirty flag
        uint64_t ticket = 0;
        std::shared_ptr<render::ComputeSpan> span;   // its GPU time, when timestamps are there
    };
    std::deque<InFlight> inFlight;
};

ev::HostClass g_cacheClass;

Cache* cacheOf(Value self, const char* what) {
    auto* c = static_cast<Cache*>(g_cacheClass.unwrap(self));
    if (!c) ev::throwTypeError(std::string(what) + ": not a TileCache");
    if (c->destroyed) ev::throwTypeError(std::string(what) + ": the TileCache was destroyed");
    return c;
}

// Its GPU memory, released once the work in flight is done: every pool and
// table it made. Its KernelBuffers throw from then on, as a destroyed one's do.
void destroyCache(Cache& c) {
    c.destroyed = true;
    for (Layer& l : c.layers) l.pool->destroy();
    for (auto* b : {&c.drawList, &c.lookup, &c.table, &c.rangePool, &c.detailPool})
        if (*b) (*b)->destroy();
    c.reports.clear();
    c.inFlight.clear();
    c.invalidatedLayers.clear();
    c.tree.clear();
}

double numberOr(Value obj, const char* key, double fallback) {
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : fallback;
}

// `n` numbers of an array property; false when it is missing or short.
bool numbers(Value obj, const char* key, double* out, uint32_t n) {
    const Rooted arr(ev::getProperty(obj, key));
    if (!ev::isObject(arr)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        Value v = ev::getElement(arr, i);
        if (!ev::isNumber(v)) return false;
        out[i] = ev::toDouble(v);
    }
    return true;
}

uint32_t integer(Value obj, const char* key, double fallback, double lo, double hi) {
    const double v = numberOr(obj, key, fallback);
    if (!(v >= lo && v <= hi) || v != std::floor(v))
        ev::throwRangeError(std::string("bro.tiles.createCache: ") + key + " is an integer in [" +
                            std::to_string(static_cast<long long>(lo)) + ", " +
                            std::to_string(static_cast<long long>(hi)) + "]");
    return static_cast<uint32_t>(v);
}

Value vec3(const tilecache::Vec3d& v) {
    ev::ArrayBuilder a(3);
    a.set(0, v.x);
    a.set(1, v.y);
    a.set(2, v.z);
    return a.get();
}

// The ranges and detail producers wrote, for the readbacks the GPU has
// finished: each tile still in the slot it was produced into gets its own.
void collectReports(Cache& c) {
    while (!c.reports.empty() && c.reports.front().ready()) {
        const Cache::Report& rb = c.reports.front();
        const auto* ranges = rb.ranges ? static_cast<const float*>(rb.ranges->data()) : nullptr;
        const auto* detail = rb.detail ? static_cast<const float*>(rb.detail->data()) : nullptr;
        for (const tilecache::Production& p : rb.tiles) {
            if (c.tree.slotOf(p.id) != static_cast<int32_t>(p.slot)) continue;
            const uint32_t k = p.slot - rb.firstSlot;
            if (ranges && std::isfinite(ranges[2 * k]) && std::isfinite(ranges[2 * k + 1]))
                c.tree.setRange(p.id, {ranges[2 * k], ranges[2 * k + 1]});
            if (detail && std::isfinite(detail[k])) c.tree.setDetail(p.id, detail[k]);
        }
        c.reports.pop_front();
    }
}

Value tileObject(Cache& c, const tilecache::Production& p) {
    const tilecache::Domain& d = c.tree.config().domain;
    ev::ObjectBuilder t;
    t.set("root", p.id.root);
    t.set("level", p.id.level);
    t.set("x", p.id.x);
    t.set("y", p.id.y);
    t.set("slot", p.slot);
    t.set("cells", c.cells);
    t.set("border", c.border);
    t.set("tileSize", c.tree.config().tileSize);
    t.set("extent", d.tileExtent(p.id));
    t.set("origin", vec3(d.point(p.id, 0, 0)));
    const tilecache::Domain::Frame f = d.frame(p.id);
    ev::ObjectBuilder frame;
    frame.set("corner", vec3(f.corner));
    frame.set("u", vec3(f.du));
    frame.set("v", vec3(f.dv));
    t.set("frame", frame.get());
    const tilecache::Bounds b = c.tree.bounds(p.id);
    t.set("center", vec3(b.center));
    t.set("radius", b.radius);
    // The batch table's anchor (f32 values): what a producer anchors its own
    // f64 work at on the CPU, so it lines up with the table's offsets.
    const TileAnchor a = anchorOf(d, f, b.center);
    ev::ObjectBuilder anchor;
    anchor.set("base", vec3({a.base[0], a.base[1], a.base[2]}));
    anchor.set("dir", vec3({a.dirHi[0], a.dirHi[1], a.dirHi[2]}));
    t.set("anchor", anchor.get());
    if (p.parentSlot >= 0) {
        ev::ObjectBuilder parent;
        parent.set("slot", p.parentSlot);
        // The quarter of the parent this tile covers: its interior starts at
        // quadrant * tileSize / 2 cells into the parent's interior.
        ev::ArrayBuilder q(2);
        q.set(0, p.id.x & 1u);
        q.set(1, p.id.y & 1u);
        parent.set("quadrant", q.get());
        t.set("parent", parent.get());
    } else {
        t.setNull("parent");
    }
    return t.get();
}

void writeLookup(Cache& c) {
    const uint32_t n = lookupEntries(c.tree.config().capacity);
    std::vector<int32_t> words(size_t{4} * (n + 1), 0);
    words[0] = static_cast<int32_t>(n - 1);
    for (uint32_t i = 1; i <= n; ++i) words[4 * i + 2] = words[4 * i + 3] = -1;
    for (const tilecache::Production& t : c.tree.readyTiles()) {
        uint32_t h = tilecache::lookupHash(t.id) & (n - 1);
        while (words[4 * (h + 1) + 3] >= 0) h = (h + 1) & (n - 1);
        int32_t* e = words.data() + 4 * (h + 1);
        e[0] = static_cast<int32_t>(t.id.x);
        e[1] = static_cast<int32_t>(t.id.y);
        const int32_t parent = t.id.level > 0 ? c.tree.slotOf(t.id.parent()) : -1;
        e[2] = static_cast<int32_t>(t.id.root | (t.id.level << 8)) | ((parent + 1) << 13);
        e[3] = static_cast<int32_t>(t.slot);
    }
    std::string err;
    c.lookup->write(0, words.data(), words.size() * sizeof(int32_t), &err);
}

void writeDrawList(Cache& c) {
    c.drawn = c.tree.drawSet();
    if (c.drawn.empty()) return;
    std::vector<int32_t> records(c.drawn.size() * kRecordInts, 0);
    for (size_t i = 0; i < c.drawn.size(); ++i) {
        const DrawTile& d = c.drawn[i];
        int32_t* r = records.data() + i * kRecordInts;
        r[0] = static_cast<int32_t>(d.slot);
        r[1] = d.parentSlot;
        r[2] = static_cast<int32_t>(d.id.root);
        r[3] = static_cast<int32_t>(d.id.level);
        r[4] = static_cast<int32_t>(d.id.x);
        r[5] = static_cast<int32_t>(d.id.y);
        r[6] = d.coarser[0] | (d.coarser[1] << 4) | (d.coarser[2] << 8) | (d.coarser[3] << 12);
        float f[8] = {};
        const double c[3] = {d.bounds.center.x, d.bounds.center.y, d.bounds.center.z};
        for (int k = 0; k < 3; ++k) {
            f[k] = static_cast<float>(c[k]);
            f[4 + k] = static_cast<float>(c[k] - static_cast<double>(f[k]));
        }
        f[3] = static_cast<float>(d.bounds.radius);
        std::memcpy(r + 8, f, sizeof(f));
    }
    // The buffer holds the last list written: the same one again (a view
    // that draws what it drew) is not uploaded.
    if (records == c.drawRecords) return;
    std::string err;
    c.drawList->write(0, records.data(), records.size() * sizeof(int32_t), &err);
    c.drawRecords = std::move(records);
}

// What the next batch may spend, from the milliseconds the frame leaves its
// refinement (Engine::refineBudgetMs: production runs in the gap after the
// frame's work) and what this cache's tiles cost.
engine::Engine* g_engine = nullptr;
void pace(Cache& c) {
    const double budget = g_engine ? g_engine->refineBudgetMs() : -1.0;
    c.spendMs = std::numeric_limits<double>::infinity();
    if (budget < 0.0) {
        c.batchSize = c.producePerUpdate;
        return;
    }
    if (c.tileMs <= 0.0) {
        c.batchSize = (c.producePerUpdate + 1) / 2;
        return;
    }
    c.wantMs = c.tree.pending() * c.tileMs;
    double total = 0.0;
    for (const Cache* o : s_live) total += o->wantMs;
    const double share = total > 0.0 ? budget * c.wantMs / total : 0.0;
    double dearest = c.tileMs;
    for (const double ms : c.levelMs) dearest = std::max(dearest, ms);
    c.credit = std::min(c.credit + share, share + dearest);
    c.spendMs = c.credit;
    c.batchSize = c.producePerUpdate;
}

// A retired batch's span into the cache's costs: the mean a tile, and each
// level in the batch toward its estimate scaled by how far the batch's
// estimates missed, by the share of the estimate the level held.
void timeBatch(Cache& c, const Cache::InFlight& f) {
    if (!f.span || !f.span->ready() || f.tiles.empty()) return;
    const double spanMs = f.span->ms();
    if (spanMs <= 0.0) return;
    const double n = static_cast<double>(f.tiles.size());
    constexpr double kKeep = 0.9;
    c.fitW = kKeep * c.fitW + 1.0;
    c.fitN = kKeep * c.fitN + n;
    c.fitM = kKeep * c.fitM + spanMs;
    c.fitNN = kKeep * c.fitNN + n * n;
    c.fitNM = kKeep * c.fitNM + n * spanMs;
    const double meanN = c.fitN / c.fitW, meanM = c.fitM / c.fitW;
    const double varN = c.fitNN / c.fitW - meanN * meanN;
    if (varN > 0.25) {
        const double slope = (c.fitNM / c.fitW - meanN * meanM) / varN;
        // A span that does not grow with its tiles is all overhead.
        c.batchMs = slope > 0.0 ? std::max(0.0, meanM - slope * meanN) : meanM;
    }
    // The tiles' share of the span; at least a tenth of it, whatever the fit.
    const double ms = std::max(spanMs - c.batchMs, 0.1 * spanMs);
    const double perTile = ms / n;
    if (c.tileMs <= 0.0) c.tileMs = perTile;
    std::vector<std::pair<uint32_t, double>> byLevel;   // level, its estimated ms in the batch
    double predicted = 0.0;
    for (const tilecache::Production& p : f.tiles) {
        const double est = c.costOf(p.id.level);
        predicted += est;
        auto it = std::find_if(byLevel.begin(), byLevel.end(), [&](const auto& e) { return e.first == p.id.level; });
        if (it == byLevel.end()) byLevel.push_back({p.id.level, est});
        else it->second += est;
    }
    const double scale = ms / predicted;
    for (const auto& [level, est] : byLevel) {
        if (level >= c.levelMs.size()) c.levelMs.resize(level + 1, -1.0);
        c.levelMs[level] = c.costOf(level) * std::pow(scale, 0.5 * est / predicted);
    }
    c.tileMs = 0.8 * c.tileMs + 0.2 * perTile;
}

// The batches the GPU has finished, ready: their slots logged on the layer
// pools (a texture array filled from one copies those alone, respecting
// selective layer invalidation masks), and the graphics queue told to wait
// for them before it reads any. True when any were.
bool retireBatches(Cache& c) {
    render::VulkanCompute* compute = engineCompute();
    bool any = false;
    std::vector<std::vector<uint32_t>> layerSlots(c.layers.size());
    while (!c.inFlight.empty() && (!compute || compute->backgroundComplete(c.inFlight.front().ticket))) {
        const Cache::InFlight& f = c.inFlight.front();
        for (const tilecache::Production& p : f.tiles) {
            c.tree.markReady(p.id);
            for (size_t i = 0; i < c.layers.size(); ++i) {
                if (f.layerMask.empty() || (i < f.layerMask.size() && f.layerMask[i]))
                    layerSlots[i].push_back(p.slot);
            }
        }
        if (compute) compute->acquireBackground(f.ticket);
        timeBatch(c, f);
        c.inFlight.pop_front();
        any = true;
    }
    for (size_t i = 0; i < c.layers.size(); ++i) {
        if (!layerSlots[i].empty())
            if (auto buffer = computeBufferOf(c.layers[i].pool))
                buffer->markLayers(layerSlots[i], c.tree.config().capacity);
    }
    return any;
}

Value js_update(Value self, std::span<const Value> args) {
    if (render::VulkanCompute* compute = engineCompute(); compute && compute->inBackground())
        return ev::throwTypeError("TileCache.update: inside bro.kernel.background (production runs there already)");
    Cache* c = cacheOf(self, "TileCache.update");
    const Rooted selfRoot(self);
    const Rooted view(args.empty() ? ev::undefined() : args[0]);
    double pos[3];
    if (!ev::isObject(view) || !numbers(view, "position", pos, 3))
        return ev::throwTypeError("TileCache.update: takes { position: [x, y, z], fov, viewportHeight }");
    const double fovDeg = std::clamp(numberOr(view, "fov", 60.0), 1.0, 179.0);
    const double height = numberOr(view, "viewportHeight", 1080.0);
    tilecache::View v;
    v.position = {pos[0], pos[1], pos[2]};
    const double tanHalf = std::tan(fovDeg * 3.14159265358979323846 / 360.0);
    v.projScale = height / (2.0 * tanHalf);
    double fwd[3], up[3] = {0, 1, 0};
    if (numbers(view, "forward", fwd, 3)) {
        numbers(view, "up", up, 3);
        auto unit = [](tilecache::Vec3d a) {
            const double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
            return l > 0 ? tilecache::Vec3d{a.x / l, a.y / l, a.z / l} : a;
        };
        auto cross = [](const tilecache::Vec3d& a, const tilecache::Vec3d& b) {
            return tilecache::Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        v.forward = unit({fwd[0], fwd[1], fwd[2]});
        v.right = unit(cross(v.forward, {up[0], up[1], up[2]}));
        v.up = cross(v.right, v.forward);
        v.tanHalfY = tanHalf;
        v.tanHalfX = tanHalf * numberOr(view, "viewportWidth", height * 16.0 / 9.0) / height;
        v.frustum = true;
    }
    // focus: [{ position, radius, cell }]: places wanted whatever is seen.
    const Rooted focus(ev::getProperty(view, "focus"));
    if (ev::isObject(focus)) {
        const double n = numberOr(focus, "length", 0);
        for (uint32_t i = 0; i < n; ++i) {
            const Rooted f(ev::getElement(focus, i));
            double fp[3];
            if (!ev::isObject(f) || !numbers(f, "position", fp, 3))
                return ev::throwTypeError("TileCache.update: a focus is { position: [x, y, z], radius, cell }");
            const double radius = numberOr(f, "radius", 0), cell = numberOr(f, "cell", 0);
            if (!(radius >= 0) || !(cell > 0))
                return ev::throwRangeError("TileCache.update: a focus's radius is >= 0 and its cell > 0");
            v.focus.push_back({{fp[0], fp[1], fp[2]}, radius, cell});
        }
    }
    // Ready first: a batch's reports come back with it, and the tree takes
    // them for a ready tile.
    const bool retired = retireBatches(*c);
    collectReports(*c);
    const uint32_t dropped = c->tree.select(v);

    pace(*c);
    const Cache* costs = c;
    const std::vector<tilecache::Production> batch =
        std::isfinite(c->spendMs)
            ? c->tree.takeProductions(c->batchSize, c->spendMs, [costs](uint32_t level) { return costs->costOf(level); })
            : c->tree.takeProductions(c->batchSize);
    for (const tilecache::Production& p : batch)
        if (std::isfinite(c->spendMs)) c->credit -= c->costOf(p.id.level);

    // Check whether all tiles in the batch are in-place refreshes of ready slots.
    bool allRefresh = !batch.empty();
    for (const tilecache::Production& p : batch) {
        if (c->tree.slotOf(p.id) != static_cast<int32_t>(p.slot)) {
            allRefresh = false;
            break;
        }
    }
    const bool selective = allRefresh && !c->invalidatedLayers.empty();
    std::vector<std::string> activeLayers;
    std::vector<bool> batchMask;
    if (selective) {
        activeLayers = c->invalidatedLayers;
        batchMask.resize(c->layers.size(), false);
        for (size_t i = 0; i < c->layers.size(); ++i)
            batchMask[i] = std::find(activeLayers.begin(), activeLayers.end(), c->layers[i].name) != activeLayers.end();
    } else {
        activeLayers.reserve(c->layers.size());
        for (const Layer& l : c->layers) activeLayers.push_back(l.name);
    }
    if (!batch.empty() && c->tree.stale() == 0) c->invalidatedLayers.clear();

    render::VulkanCompute* compute = engineCompute();
    // The lookup without the tiles whose slots the batch takes (or that
    // select dropped out of date), before the batch: its work waits for what
    // is submitted before it, so no ray reads a slot as it is rewritten.
    if (retired || dropped || !batch.empty()) writeLookup(*c);
    std::shared_ptr<render::ComputeSpan> span;
    if (!batch.empty() && compute) {
        compute->beginBackground();
        span = compute->beginSpan();
    }
    if (!batch.empty()) {
        std::string err;
        // A producer that writes no range or detail leaves NaN, which is no
        // report: a slot's last tile's must not stand in for it. Skip wiping
        // during a selective layer refresh so existing height ranges stay intact.
        if (!selective) {
            const float none[2] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
            if (c->rangePool)
                for (const tilecache::Production& p : batch) c->rangePool->write(size_t{8} * p.slot, none, sizeof(none), &err);
            if (c->detailPool)
                for (const tilecache::Production& p : batch) c->detailPool->write(size_t{4} * p.slot, none, sizeof(float), &err);
        }
        // The batch on the GPU, for kernels that produce it in one dispatch.
        const tilecache::Domain& d = c->tree.config().domain;
        std::vector<int32_t> records(batch.size() * kTableWords, 0);
        for (size_t i = 0; i < batch.size(); ++i) {
            const tilecache::Production& p = batch[i];
            int32_t* r = records.data() + i * kTableWords;
            const tilecache::Domain::Frame f = d.frame(p.id);
            const tilecache::Bounds b = c->tree.bounds(p.id);
            const TileAnchor a = anchorOf(d, f, b.center);
            const int32_t ints[8] = {static_cast<int32_t>(p.slot), p.parentSlot, static_cast<int32_t>(p.id.root),
                                     static_cast<int32_t>(p.id.level), static_cast<int32_t>(p.id.x),
                                     static_cast<int32_t>(p.id.y), static_cast<int32_t>(p.id.x & 1u),
                                     static_cast<int32_t>(p.id.y & 1u)};
            std::copy(std::begin(ints), std::end(ints), r);
            const float floats[28] = {a.base[0], a.base[1], a.base[2], static_cast<float>(d.tileExtent(p.id)),
                                      a.lost[0], a.lost[1], a.lost[2], static_cast<float>(b.radius),
                                      static_cast<float>(f.du.x), static_cast<float>(f.du.y), static_cast<float>(f.du.z), 0.0f,
                                      static_cast<float>(f.dv.x), static_cast<float>(f.dv.y), static_cast<float>(f.dv.z), 0.0f,
                                      a.dirHi[0], a.dirHi[1], a.dirHi[2], 0.0f,
                                      a.dirLo[0], a.dirLo[1], a.dirLo[2], 0.0f,
                                      a.point[0], a.point[1], a.point[2], 0.0f};
            std::memcpy(r + 8, floats, sizeof(floats));
        }
        c->table->write(0, records.data(), records.size() * sizeof(int32_t), &err);

        ev::ArrayBuilder tiles(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) tiles.set(static_cast<uint32_t>(i), tileObject(*c, batch[i]));
        ev::ArrayBuilder layersArr(activeLayers.size());
        for (size_t i = 0; i < activeLayers.size(); ++i)
            layersArr.set(static_cast<uint32_t>(i), activeLayers[i]);
        ev::ObjectBuilder b;
        b.set("count", static_cast<double>(batch.size()));
        b.set("tiles", tiles.get());
        b.set("table", c->tableValue.get());
        b.set("layers", layersArr.get());
        b.def("hasLayer", 1, [](Value bSelf, std::span<const Value> hargs) -> Value {
            if (hargs.empty()) return ev::fromBool(false);
            const std::string want = ev::toUtf8(hargs[0]);
            const Rooted arr(ev::getProperty(bSelf, "layers"));
            if (!ev::isObject(arr)) return ev::fromBool(false);
            const double n = numberOr(arr, "length", 0);
            for (uint32_t i = 0; i < n; ++i)
                if (ev::toUtf8(ev::getElement(arr, i)) == want) return ev::fromBool(true);
            return ev::fromBool(false);
        });
        const Rooted arg(b.get());
        const Rooted produce(ev::getProperty(selfRoot, "produce"));
        const Value argv = arg;
        const ev::CallResult r = ev::call(produce, selfRoot, std::span<const Value>(&argv, 1));
        if (r.thrown) {
            // The batch stays missing; the next update offers it again.
            const Rooted thrown(r.value);
            if (selective && c->invalidatedLayers.empty()) c->invalidatedLayers = activeLayers;
            for (const tilecache::Production& p : batch) c->tree.abandon(p.id);
            if (compute) compute->endBackground();
            writeDrawList(*c);
            return ev::throwValue(thrown);
        }
    }
    if ((c->rangePool || c->detailPool) && !batch.empty()) {
        Cache::Report rb;
        uint32_t last = 0;
        rb.firstSlot = UINT32_MAX;
        for (const tilecache::Production& p : batch) {
            rb.firstSlot = std::min(rb.firstSlot, p.slot);
            last = std::max(last, p.slot);
        }
        const size_t n = last - rb.firstSlot + 1;
        if (c->rangePool) rb.ranges = engineReadLater(c->rangePool, size_t{8} * rb.firstSlot, size_t{8} * n);
        if (c->detailPool) rb.detail = engineReadLater(c->detailPool, size_t{4} * rb.firstSlot, size_t{4} * n);
        rb.tiles = batch;
        if ((!c->rangePool || rb.ranges) && (!c->detailPool || rb.detail)) c->reports.push_back(std::move(rb));
    }
    if (!batch.empty()) {
        // Ready once the GPU has done it (retireBatches, a later update).
        Cache::InFlight f;
        f.tiles = batch;
        f.layerMask = std::move(batchMask);
        if (span) compute->endSpan(*span);
        f.span = std::move(span);
        f.ticket = compute ? compute->endBackground() : 0;
        c->inFlight.push_back(std::move(f));
    }
    writeDrawList(*c);
    return ev::fromDouble(static_cast<double>(batch.size()));
}

Value drawnTiles(const Cache* c, const std::vector<uint32_t>* only);

Value js_drawn(Value self, std::span<const Value>) { return drawnTiles(cacheOf(self, "TileCache.drawn"), nullptr); }

// drawnNear([x, y, z], reach): the drawn tiles whose footprint on the base
// surface (the bounding sphere less its height range) comes within `reach`
// of the point.
Value js_drawnNear(Value self, std::span<const Value> args) {
    const Cache* c = cacheOf(self, "TileCache.drawnNear");
    double p[3];
    bool ok = !args.empty() && ev::isObject(args[0]);
    for (uint32_t k = 0; ok && k < 3; ++k) {
        const Value v = ev::getElement(args[0], k);
        ok = ev::isNumber(v);
        if (ok) p[k] = ev::toDouble(v);
    }
    const double reach = args.size() > 1 && ev::isNumber(args[1]) ? ev::toDouble(args[1]) : -1.0;
    if (!ok || !(reach >= 0)) return ev::throwTypeError("TileCache.drawnNear: takes [x, y, z] and a reach >= 0");
    const bool plane = c->tree.config().domain.kind == tilecache::Domain::Kind::Plane;
    std::vector<uint32_t> near;
    for (uint32_t i = 0; i < c->drawn.size(); ++i) {
        const DrawTile& d = c->drawn[i];
        const tilecache::HeightRange r = c->tree.range(d.id);
        const double h = std::max(std::abs(r.lo), std::abs(r.hi));
        const double foot = plane ? std::sqrt(std::max(0.0, d.bounds.radius * d.bounds.radius - h * h))
                                  : std::max(0.0, d.bounds.radius - h);
        const double dx = p[0] - d.bounds.center.x, dy = p[1] - d.bounds.center.y, dz = p[2] - d.bounds.center.z;
        const double lim = foot + reach;
        if (dx * dx + dy * dy + dz * dz <= lim * lim) near.push_back(i);
    }
    return drawnTiles(c, &near);
}

Value drawnTiles(const Cache* c, const std::vector<uint32_t>* only) {
    const uint32_t n = static_cast<uint32_t>(only ? only->size() : c->drawn.size());
    ev::ArrayBuilder out(n);
    for (uint32_t i = 0; i < n; ++i) {
        const DrawTile& d = c->drawn[only ? (*only)[i] : i];
        ev::ObjectBuilder t;
        t.set("root", d.id.root);
        t.set("level", d.id.level);
        t.set("x", d.id.x);
        t.set("y", d.id.y);
        t.set("slot", d.slot);
        t.set("parentSlot", d.parentSlot);
        ev::ArrayBuilder coarser(4);
        for (uint32_t e = 0; e < 4; ++e) coarser.set(e, static_cast<uint32_t>(d.coarser[e]));
        t.set("coarser", coarser.get());
        t.set("center", vec3(d.bounds.center));
        t.set("radius", d.bounds.radius);
        const tilecache::HeightRange r = c->tree.range(d.id);
        ev::ArrayBuilder range(2);
        range.set(0, r.lo);
        range.set(1, r.hi);
        t.set("range", range.get());
        out.set(i, t.get());
    }
    return out.get();
}

Value js_invalidate(Value self, std::span<const Value> args) {
    Cache* c = cacheOf(self, "TileCache.invalidate");
    if (args.empty() || ev::isNull(args[0]) || ev::isUndefined(args[0])) {
        c->invalidatedLayers.clear();
        c->tree.refresh();
        return ev::undefined();
    }
    std::vector<std::string> wanted;
    auto addLayer = [&](const std::string& name) -> bool {
        bool found = false;
        for (const Layer& l : c->layers) {
            if (l.name == name) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        if (std::find(wanted.begin(), wanted.end(), name) == wanted.end())
            wanted.push_back(name);
        return true;
    };
    if (ev::isString(args[0])) {
        const std::string name = ev::toUtf8(args[0]);
        if (!addLayer(name))
            return ev::throwRangeError("TileCache.invalidate: no layer '" + name + "'");
    } else if (ev::isObject(args[0])) {
        const Rooted arr(args[0]);
        const double n = numberOr(arr, "length", 0);
        for (uint32_t i = 0; i < n; ++i) {
            const std::string name = ev::toUtf8(ev::getElement(arr, i));
            if (!addLayer(name))
                return ev::throwRangeError("TileCache.invalidate: no layer '" + name + "'");
        }
    } else {
        return ev::throwTypeError("TileCache.invalidate: layers must be a layer name or array of layer names");
    }
    c->invalidatedLayers = std::move(wanted);
    c->tree.refresh();
    return ev::undefined();
}

void decorateCache(ev::ObjectBuilder& b) {
    b.def("update", 1, js_update);
    b.def("layer", 1, [](Value self, std::span<const Value> args) -> Value {
        const Cache* c = cacheOf(self, "TileCache.layer");
        const std::string name = args.empty() ? std::string() : ev::toUtf8(args[0]);
        for (const Layer& l : c->layers)
            if (l.name == name) return brokernel::api::bufferValue(l.pool);
        return ev::throwRangeError("TileCache.layer: no layer '" + name + "'");
    });
    b.def("clear", 0, [](Value self, std::span<const Value>) -> Value {
        Cache* c = cacheOf(self, "TileCache.clear");
        c->tree.clear();
        c->drawn.clear();
        c->reports.clear();
        c->inFlight.clear();   // their slots are free; later batches run after them
        c->invalidatedLayers.clear();
        writeLookup(*c);
        return ev::undefined();
    });
    b.def("destroy", 0, [](Value self, std::span<const Value>) -> Value {
        destroyCache(*cacheOf(self, "TileCache.destroy"));
        return ev::undefined();
    });
    b.def("refresh", 1, js_invalidate);
    b.def("invalidate", 1, js_invalidate);
    b.accessor("stale", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(static_cast<double>(cacheOf(self, "TileCache.stale")->tree.stale()));
    });
    b.accessor("lookup", [](Value self, std::span<const Value>) -> Value {
        return brokernel::api::bufferValue(cacheOf(self, "TileCache.lookup")->lookup);
    });
    b.accessor("ranges", [](Value self, std::span<const Value>) -> Value {
        const Cache* c = cacheOf(self, "TileCache.ranges");
        return c->rangePool ? brokernel::api::bufferValue(c->rangePool) : ev::null();
    });
    b.accessor("detail", [](Value self, std::span<const Value>) -> Value {
        const Cache* c = cacheOf(self, "TileCache.detail");
        return c->detailPool ? brokernel::api::bufferValue(c->detailPool) : ev::null();
    });
    b.accessor("drawn", js_drawn);
    b.def("drawnNear", 2, js_drawnNear);
    b.accessor("drawList", [](Value self, std::span<const Value>) -> Value {
        return brokernel::api::bufferValue(cacheOf(self, "TileCache.drawList")->drawList);
    });
    b.accessor("drawCount", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(static_cast<double>(cacheOf(self, "TileCache.drawCount")->drawn.size()));
    });
    b.accessor("pending", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(static_cast<double>(cacheOf(self, "TileCache.pending")->tree.pending()));
    });
    b.accessor("wanted", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(static_cast<double>(cacheOf(self, "TileCache.wanted")->tree.wanted()));
    });
    b.accessor("resident", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(static_cast<double>(cacheOf(self, "TileCache.resident")->tree.resident()));
    });
    b.accessor("cells", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(cacheOf(self, "TileCache.cells")->cells);
    });
    b.accessor("capacity", [](Value self, std::span<const Value>) -> Value {
        return ev::fromDouble(cacheOf(self, "TileCache.capacity")->tree.config().capacity);
    });
    b.accessor("stats", [](Value self, std::span<const Value>) -> Value {
        const Cache* c = cacheOf(self, "TileCache.stats");
        const tilecache::Stats& s = c->tree.stats();
        ev::ObjectBuilder o;
        o.set("produced", static_cast<double>(s.produced));
        o.set("evicted", static_cast<double>(s.evicted));
        o.set("reportsInFlight", static_cast<double>(c->reports.size()));
        o.set("batchSize", static_cast<double>(c->batchSize));
        o.set("tileMs", c->tileMs);
        o.set("batchMs", c->batchMs);
        ev::ArrayBuilder levels(static_cast<uint32_t>(c->levelMs.size()));
        for (uint32_t i = 0; i < c->levelMs.size(); ++i) levels.set(i, c->levelMs[i]);
        o.set("levelMs", levels.get());
        return o.get();
    });
}

// Object.keys(obj): the layer table's names, in its own order.
std::vector<std::string> keysOf(Value obj) {
    const Rooted target(obj);
    const Rooted object(ev::globalValue("Object").value);
    const Rooted keys(ev::getProperty(object, "keys"));
    const Value arg = target;
    const ev::CallResult r = ev::call(keys, object, std::span<const Value>(&arg, 1));
    if (r.thrown) ev::throwValue(r.value);
    const Rooted arr(r.value);
    std::vector<std::string> out;
    const double n = numberOr(arr, "length", 0);
    for (uint32_t i = 0; i < n; ++i) out.push_back(ev::toUtf8(ev::getElement(arr, i)));
    return out;
}

Value js_createCache(Value, std::span<const Value> args) {
    const Rooted opts(args.empty() ? ev::undefined() : args[0]);
    if (!ev::isObject(opts)) return ev::throwTypeError("bro.tiles.createCache: takes an options object");
    const Rooted produce(ev::getProperty(opts, "produce"));
    if (!ev::isFunction(produce)) return ev::throwTypeError("bro.tiles.createCache: `produce(batch)` is a function");

    tilecache::Config cfg;
    const Rooted domain(ev::getProperty(opts, "domain"));
    const std::string type = ev::isObject(domain) ? ev::toUtf8(ev::getProperty(domain, "type")) : std::string();
    if (type == "plane") {
        cfg.domain.kind = tilecache::Domain::Kind::Plane;
        numbers(domain, "origin", cfg.domain.origin, 2);
        cfg.domain.size = numberOr(domain, "size", 1.0);
        double roots[2] = {1, 1};
        numbers(domain, "roots", roots, 2);
        if (!(cfg.domain.size > 0) || roots[0] < 1 || roots[1] < 1 || roots[0] * roots[1] > 255 ||
            roots[0] != std::floor(roots[0]) || roots[1] != std::floor(roots[1]))
            return ev::throwRangeError("bro.tiles.createCache: a plane has a positive size and 1 to 255 roots");
        cfg.domain.rootsX = static_cast<uint32_t>(roots[0]);
        cfg.domain.rootsY = static_cast<uint32_t>(roots[1]);
    } else if (type == "cubeSphere") {
        cfg.domain.kind = tilecache::Domain::Kind::CubeSphere;
        cfg.domain.radius = numberOr(domain, "radius", 1.0);
        double center[3] = {0, 0, 0};
        numbers(domain, "center", center, 3);
        cfg.domain.center = {center[0], center[1], center[2]};
        if (!(cfg.domain.radius > 0)) return ev::throwRangeError("bro.tiles.createCache: a cube-sphere has a positive radius");
    } else {
        return ev::throwTypeError("bro.tiles.createCache: domain.type is 'plane' or 'cubeSphere'");
    }
    cfg.tileSize = integer(opts, "tileSize", 64, 1, 4096);
    cfg.capacity = integer(opts, "capacity", 256, 1, 65536);
    cfg.maxLevel = integer(opts, "maxLevel", 16, 0, tilecache::TileId::kMaxLevel);
    cfg.maxHeight = numberOr(opts, "maxHeight", 0.0);
    cfg.minHeight = numberOr(opts, "minHeight", std::min(0.0, cfg.maxHeight));
    if (!(cfg.minHeight <= cfg.maxHeight))
        return ev::throwRangeError("bro.tiles.createCache: minHeight is at most maxHeight");
    cfg.pixelsPerCell = numberOr(opts, "pixelsPerCell", 2.0);
    if (!(cfg.pixelsPerCell > 0)) return ev::throwRangeError("bro.tiles.createCache: pixelsPerCell is positive");
    const bool detail = ev::toBool(ev::getProperty(opts, "detail"));
    cfg.pixelsPerDetail = detail ? numberOr(opts, "pixelsPerDetail", 1.0) : 0.0;
    if (detail && !(cfg.pixelsPerDetail > 0))
        return ev::throwRangeError("bro.tiles.createCache: pixelsPerDetail is positive");

    auto cache = std::make_unique<Cache>(cfg);
    cache->border = integer(opts, "border", 0, 0, 64);
    cache->cells = cfg.tileSize + 2 * cache->border;
    cache->producePerUpdate = integer(opts, "producePerUpdate", 8, 1, 65536);

    const Rooted layers(ev::getProperty(opts, "layers"));
    const std::vector<std::string> names = ev::isObject(layers) ? keysOf(layers) : std::vector<std::string>{};
    if (names.empty())
        return ev::throwTypeError("bro.tiles.createCache: `layers` names at least one layer: { name: bytesPerCell }");
    std::string err;
    for (const std::string& name : names) {
        Layer l;
        l.name = name;
        const double bytes = numberOr(layers, name.c_str(), 0);
        if (!(bytes >= 4 && bytes <= 4096) || std::fmod(bytes, 4) != 0)
            return ev::throwRangeError("bro.tiles.createCache: layer '" + name +
                                       "' has 4 to 4096 bytes a cell, a multiple of 4");
        l.cellBytes = static_cast<uint32_t>(bytes);
        l.tileBytes = static_cast<size_t>(cache->cells) * cache->cells * l.cellBytes;
        l.pool = createEngineBuffer(l.tileBytes * cfg.capacity, &err);
        if (!l.pool) return ev::throwError("bro.tiles.createCache: " + err);
        cache->layers.push_back(std::move(l));
    }
    cache->drawList = createEngineBuffer(static_cast<size_t>(cfg.capacity) * kRecordInts * sizeof(int32_t), &err);
    if (!cache->drawList) return ev::throwError("bro.tiles.createCache: " + err);
    cache->lookup = createEngineBuffer(size_t{16} * (lookupEntries(cfg.capacity) + 1), &err);
    if (!cache->lookup) return ev::throwError("bro.tiles.createCache: " + err);
    writeLookup(*cache);
    if (ev::toBool(ev::getProperty(opts, "ranges"))) {
        cache->rangePool = createEngineBuffer(size_t{8} * cfg.capacity, &err);
        if (!cache->rangePool) return ev::throwError("bro.tiles.createCache: " + err);
    }
    if (detail) {
        cache->detailPool = createEngineBuffer(size_t{4} * cfg.capacity, &err);
        if (!cache->detailPool) return ev::throwError("bro.tiles.createCache: " + err);
    }

    cache->table = createEngineBuffer(size_t{kTableWords} * sizeof(int32_t) * cache->producePerUpdate, &err);
    if (!cache->table) return ev::throwError("bro.tiles.createCache: " + err);
    cache->tableValue.set(brokernel::api::bufferValue(cache->table));

    // `produce` lives on the object, so the collector traces it with the cache.
    const Rooted obj(g_cacheClass.createInstance(std::move(cache)));
    ev::setProperty(obj, "produce", produce);
    return obj;
}

} // namespace

void installTiles(engine::Engine& engine) {
    // What the caches still owe their views, as perf.settle waits for it:
    // wanted tiles not produced, and height ranges and detail not read back
    // (the tree refines a tile's selection by them).
    static engine::Engine* s_reporting = nullptr;
    g_engine = &engine;
    if (s_reporting != &engine) {
        s_reporting = &engine;
        engine.addPendingWorkSource([](std::vector<std::string>& out) {
            uint32_t pending = 0, reports = 0;
            for (const Cache* c : s_live) {
                if (c->destroyed) continue;
                pending += c->tree.pending();
                reports += static_cast<uint32_t>(c->reports.size());
            }
            if (pending > 0) out.push_back("tiles: " + std::to_string(pending) + " wanted not produced");
            if (reports > 0) out.push_back("tiles: " + std::to_string(reports) + " batch reports in flight");
        });
    }
    brokernel::api::registerHostModule("tiles", tilecache::tilesModuleSource());
    g_cacheClass.install("TileCache", 0, nullptr, decorateCache, /*global=*/false);
    const auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    const Rooted broObj(bro.value);
    ev::ObjectBuilder tiles;
    tiles.def("createCache", 1, js_createCache);
    tiles.set("module", tilecache::tilesModuleSource());
    ev::setProperty(broObj, "tiles", tiles.get());
}

} // namespace bro::bronze_host
