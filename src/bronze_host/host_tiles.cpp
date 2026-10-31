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

// A drawList record, 16 words: int32 slot, parentSlot, root, level, x, y, 0,
// 0, then float32 centerHi.xyz, radius, centerLo.xyz, 0 (the bounding
// sphere's center as hi + lo, so a shader can subtract a camera split the
// same way and get a camera-relative position to f64 accuracy).
constexpr uint32_t kRecordInts = 16;

// A batch table record, 24 float64 (integers exact): slot, parentSlot (-1
// for a root), root, level, x, y, quadrant x, quadrant y, frame corner.xyz,
// u.xyz, v.xyz, the bounding sphere's center.xyz, extent, radius, 0, 0.
constexpr uint32_t kTableDoubles = 24;

// The lookup (common/tiles.bk): int4 records, the first [mask, 0, 0, 0],
// then mask + 1 entries, a ready tile at the first free one from
// tilecache::lookupHash(id) & mask on: [x, y, root | level << 8, slot],
// an empty one [0, 0, -1, -1]. Twice the capacity, a power of two, so a
// probe ends within a few entries.
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
    uint32_t border = 0;
    uint32_t cells = 0;          // a tile's cells along an edge, borders included
    uint32_t producePerUpdate = 8;
    std::vector<Layer> layers;
    std::shared_ptr<brokernel::api::GpuBuffer> drawList;
    std::vector<DrawTile> drawn;
    std::shared_ptr<brokernel::api::GpuBuffer> lookup;
    // The update's batch on the GPU, producePerUpdate records, and its
    // KernelBuffer, made once.
    std::shared_ptr<brokernel::api::GpuBuffer> table;
    ev::Persistent tableValue;

    // `ranges`: two float32 a slot, the [lo, hi] its producer wrote at
    // 2 * slot, read back without waiting and given to the tree.
    std::shared_ptr<brokernel::api::GpuBuffer> rangePool;
    struct RangeReadback {
        std::shared_ptr<render::ComputeReadback> readback;
        uint32_t firstSlot = 0;
        std::vector<tilecache::Production> tiles;
    };
    std::deque<RangeReadback> rangeReadbacks;

    // `produceBudgetMs`: the GPU ms an update's production may take (0: no
    // budget, every batch producePerUpdate). Each batch is timed on the GPU,
    // and the batch size follows what they took: fewer tiles after one over
    // the budget, one more after one well under it.
    double budgetMs = 0.0;
    uint32_t batchSize = 1;
    double lastBatchMs = 0.0;
    struct TimedBatch {
        std::shared_ptr<render::ComputeSpan> span;
        uint32_t count = 0;
    };
    std::deque<TimedBatch> timedBatches;
};

ev::HostClass g_cacheClass;

Cache* cacheOf(Value self, const char* what) {
    auto* c = static_cast<Cache*>(g_cacheClass.unwrap(self));
    if (!c) ev::throwTypeError(std::string(what) + ": not a TileCache");
    return c;
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

// The ranges producers wrote, for the readbacks the GPU has finished: each
// tile still in the slot it was produced into gets its range.
void collectRanges(Cache& c) {
    while (!c.rangeReadbacks.empty() && c.rangeReadbacks.front().readback->ready()) {
        const Cache::RangeReadback& rb = c.rangeReadbacks.front();
        const auto* f = static_cast<const float*>(rb.readback->data());
        for (const tilecache::Production& p : rb.tiles) {
            if (c.tree.slotOf(p.id) != static_cast<int32_t>(p.slot)) continue;
            const float* r = f + 2 * (p.slot - rb.firstSlot);
            if (std::isfinite(r[0]) && std::isfinite(r[1])) c.tree.setRange(p.id, {r[0], r[1]});
        }
        c.rangeReadbacks.pop_front();
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
        e[2] = static_cast<int32_t>(t.id.root | (t.id.level << 8));
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
        float f[8] = {};
        const double c[3] = {d.bounds.center.x, d.bounds.center.y, d.bounds.center.z};
        for (int k = 0; k < 3; ++k) {
            f[k] = static_cast<float>(c[k]);
            f[4 + k] = static_cast<float>(c[k] - static_cast<double>(f[k]));
        }
        f[3] = static_cast<float>(d.bounds.radius);
        std::memcpy(r + 8, f, sizeof(f));
    }
    std::string err;
    c.drawList->write(0, records.data(), records.size() * sizeof(int32_t), &err);
}

Value js_update(Value self, std::span<const Value> args) {
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
    collectRanges(*c);
    c->tree.select(v);

    if (c->budgetMs > 0.0) {
        while (!c->timedBatches.empty() && c->timedBatches.front().span->ready()) {
            const Cache::TimedBatch& t = c->timedBatches.front();
            const double ms = t.span->ms();
            if (ms >= 0.0) {
                c->lastBatchMs = ms;
                // A batch's cost is not proportional to its tiles (a small
                // one leaves the GPU part idle), so the size is felt for.
                if (ms > c->budgetMs)
                    c->batchSize = std::max(1u, std::min(c->batchSize, static_cast<uint32_t>(t.count * c->budgetMs / ms)));
                else if (ms < 0.85 * c->budgetMs)
                    c->batchSize = std::min(c->producePerUpdate, std::max(c->batchSize, t.count + 1));
            }
            c->timedBatches.pop_front();
        }
    }
    const uint32_t batchSize = c->budgetMs > 0.0 ? c->batchSize : c->producePerUpdate;
    const std::vector<tilecache::Production> batch = c->tree.takeProductions(batchSize);
    std::shared_ptr<render::ComputeSpan> span;
    if (!batch.empty() && c->budgetMs > 0.0)
        if (render::VulkanCompute* compute = engineCompute()) span = compute->beginSpan();
    if (!batch.empty()) {
        std::string err;
        // A producer that writes no range leaves NaN, which is no range: a
        // slot's last tile's must not stand in for it.
        if (c->rangePool) {
            const float none[2] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
            for (const tilecache::Production& p : batch) c->rangePool->write(size_t{8} * p.slot, none, sizeof(none), &err);
        }
        // The batch on the GPU, for kernels that produce it in one dispatch.
        const tilecache::Domain& d = c->tree.config().domain;
        std::vector<double> records(batch.size() * kTableDoubles, 0.0);
        for (size_t i = 0; i < batch.size(); ++i) {
            const tilecache::Production& p = batch[i];
            double* r = records.data() + i * kTableDoubles;
            const tilecache::Domain::Frame f = d.frame(p.id);
            const tilecache::Bounds b = c->tree.bounds(p.id);
            const double v[] = {static_cast<double>(p.slot), static_cast<double>(p.parentSlot),
                                static_cast<double>(p.id.root), static_cast<double>(p.id.level),
                                static_cast<double>(p.id.x), static_cast<double>(p.id.y),
                                static_cast<double>(p.id.x & 1u), static_cast<double>(p.id.y & 1u),
                                f.corner.x, f.corner.y, f.corner.z, f.du.x, f.du.y, f.du.z, f.dv.x, f.dv.y, f.dv.z,
                                b.center.x, b.center.y, b.center.z, d.tileExtent(p.id), b.radius};
            std::copy(std::begin(v), std::end(v), r);
        }
        c->table->write(0, records.data(), records.size() * sizeof(double), &err);

        ev::ArrayBuilder tiles(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) tiles.set(static_cast<uint32_t>(i), tileObject(*c, batch[i]));
        ev::ObjectBuilder b;
        b.set("count", static_cast<double>(batch.size()));
        b.set("tiles", tiles.get());
        b.set("table", c->tableValue.get());
        const Rooted arg(b.get());
        const Value argv = arg;
        const Rooted produce(ev::getProperty(selfRoot, "produce"));
        const ev::CallResult r = ev::call(produce, selfRoot, std::span<const Value>(&argv, 1));
        if (r.thrown) {
            // The batch stays missing; the next update offers it again.
            const Rooted thrown(r.value);
            for (const tilecache::Production& p : batch) c->tree.abandon(p.id);
            writeLookup(*c);
            if (span) engineCompute()->endSpan(*span);
            writeDrawList(*c);
            return ev::throwValue(thrown);
        }
        for (const tilecache::Production& p : batch) c->tree.markReady(p.id);
        // Written after the producers' dispatches, so the GPU finds a tile
        // once its cells are there; a slot taken from an evicted tile is gone
        // from it in the same write.
        writeLookup(*c);
        if (span && engineCompute()) {
            engineCompute()->endSpan(*span);
            c->timedBatches.push_back({std::move(span), static_cast<uint32_t>(batch.size())});
        }
    }
    if (c->rangePool && !batch.empty()) {
        Cache::RangeReadback rb;
        uint32_t last = 0;
        rb.firstSlot = UINT32_MAX;
        for (const tilecache::Production& p : batch) {
            rb.firstSlot = std::min(rb.firstSlot, p.slot);
            last = std::max(last, p.slot);
        }
        rb.readback = engineReadLater(c->rangePool, size_t{8} * rb.firstSlot, size_t{8} * (last - rb.firstSlot + 1));
        rb.tiles = batch;
        if (rb.readback) c->rangeReadbacks.push_back(std::move(rb));
    }
    writeDrawList(*c);
    return ev::fromDouble(static_cast<double>(batch.size()));
}

Value js_drawn(Value self, std::span<const Value>) {
    const Cache* c = cacheOf(self, "TileCache.drawn");
    ev::ArrayBuilder out(c->drawn.size());
    for (uint32_t i = 0; i < c->drawn.size(); ++i) {
        const DrawTile& d = c->drawn[i];
        ev::ObjectBuilder t;
        t.set("root", d.id.root);
        t.set("level", d.id.level);
        t.set("x", d.id.x);
        t.set("y", d.id.y);
        t.set("slot", d.slot);
        t.set("parentSlot", d.parentSlot);
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
        c->rangeReadbacks.clear();
        writeLookup(*c);
        return ev::undefined();
    });
    b.accessor("lookup", [](Value self, std::span<const Value>) -> Value {
        return brokernel::api::bufferValue(cacheOf(self, "TileCache.lookup")->lookup);
    });
    b.accessor("ranges", [](Value self, std::span<const Value>) -> Value {
        const Cache* c = cacheOf(self, "TileCache.ranges");
        return c->rangePool ? brokernel::api::bufferValue(c->rangePool) : ev::null();
    });
    b.accessor("drawn", js_drawn);
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
        o.set("rangesInFlight", static_cast<double>(c->rangeReadbacks.size()));
        o.set("batchSize", static_cast<double>(c->budgetMs > 0.0 ? c->batchSize : c->producePerUpdate));
        o.set("batchMs", c->lastBatchMs);
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

    auto cache = std::make_unique<Cache>(cfg);
    cache->border = integer(opts, "border", 0, 0, 64);
    cache->cells = cfg.tileSize + 2 * cache->border;
    cache->producePerUpdate = integer(opts, "producePerUpdate", 8, 1, 65536);
    cache->budgetMs = numberOr(opts, "produceBudgetMs", 0.0);
    if (!(cache->budgetMs >= 0.0)) return ev::throwRangeError("bro.tiles.createCache: produceBudgetMs is at least 0");

    const Rooted layers(ev::getProperty(opts, "layers"));
    const std::vector<std::string> names = ev::isObject(layers) ? keysOf(layers) : std::vector<std::string>{};
    if (names.empty())
        return ev::throwTypeError("bro.tiles.createCache: `layers` names at least one layer: { name: bytesPerCell }");
    std::string err;
    for (const std::string& name : names) {
        Layer l;
        l.name = name;
        const double bytes = numberOr(layers, name.c_str(), 0);
        if (!(bytes >= 4 && bytes <= 256) || std::fmod(bytes, 4) != 0)
            return ev::throwRangeError("bro.tiles.createCache: layer '" + name +
                                       "' has 4 to 256 bytes a cell, a multiple of 4");
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

    cache->table = createEngineBuffer(size_t{kTableDoubles} * sizeof(double) * cache->producePerUpdate, &err);
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
    // wanted tiles not produced, and height ranges not read back (the tree
    // refines a tile's selection by them).
    static engine::Engine* s_reporting = nullptr;
    if (s_reporting != &engine) {
        s_reporting = &engine;
        engine.addPendingWorkSource([](std::vector<std::string>& out) {
            uint32_t pending = 0, ranges = 0;
            for (const Cache* c : s_live) {
                pending += c->tree.pending();
                ranges += static_cast<uint32_t>(c->rangeReadbacks.size());
            }
            if (pending > 0) out.push_back("tiles: " + std::to_string(pending) + " wanted not produced");
            if (ranges > 0) out.push_back("tiles: " + std::to_string(ranges) + " height range batches in flight");
        });
    }
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
