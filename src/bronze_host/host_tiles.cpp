#include "bronze_host/host_tiles.h"

#include "bronze_host/host_kernel_gpu.h"
#include "bronze_host/host_rooted.h"
#include "tilecache/tile_tree.h"

#include "embed/embed_host_class.h"
#include "embed/embed_object_builder.h"

#include <brokernel/api.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;
using tilecache::DrawTile;

constexpr uint32_t kRecordInts = 8;   // a drawList record: slot, parentSlot, root, level, x, y, 0, 0

struct Layer {
    std::string name;
    uint32_t cellBytes = 4;
    size_t tileBytes = 0;
    std::shared_ptr<brokernel::api::GpuBuffer> pool;
};

struct Cache {
    explicit Cache(const tilecache::Config& c) : tree(c) {}
    tilecache::TileTree tree;
    uint32_t border = 0;
    uint32_t cells = 0;          // a tile's cells along an edge, borders included
    uint32_t producePerUpdate = 8;
    std::vector<Layer> layers;
    std::shared_ptr<brokernel::api::GpuBuffer> drawList;
    std::vector<DrawTile> drawn;
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

// { name: KernelBuffer } over the tile's range of each layer's pool.
Value layerViews(const Cache& c, uint32_t slot) {
    ev::ObjectBuilder b;
    for (const Layer& l : c.layers)
        b.set(l.name, brokernel::api::bufferValue(engineBufferView(l.pool, l.tileBytes * slot, l.tileBytes)));
    return b.get();
}

Value tileObject(const Cache& c, const tilecache::Production& p) {
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
    t.set("layers", layerViews(c, p.slot));
    if (p.parentSlot >= 0) {
        ev::ObjectBuilder parent;
        parent.set("slot", p.parentSlot);
        parent.set("layers", layerViews(c, static_cast<uint32_t>(p.parentSlot)));
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
    v.projScale = height / (2.0 * std::tan(fovDeg * 3.14159265358979323846 / 360.0));
    c->tree.select(v);

    const std::vector<tilecache::Production> batch = c->tree.takeProductions(c->producePerUpdate);
    const Rooted produce(ev::getProperty(selfRoot, "produce"));
    for (size_t i = 0; i < batch.size(); ++i) {
        const Rooted tile(tileObject(*c, batch[i]));
        const Value arg = tile;
        const ev::CallResult r = ev::call(produce, selfRoot, std::span<const Value>(&arg, 1));
        if (r.thrown) {
            // This tile and the rest of the batch stay missing; the next
            // update offers them again.
            const Rooted thrown(r.value);
            for (size_t j = i; j < batch.size(); ++j) c->tree.abandon(batch[j].id);
            writeDrawList(*c);
            return ev::throwValue(thrown);
        }
        c->tree.markReady(batch[i].id);
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
        return ev::undefined();
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
        const tilecache::Stats& s = cacheOf(self, "TileCache.stats")->tree.stats();
        ev::ObjectBuilder o;
        o.set("produced", static_cast<double>(s.produced));
        o.set("evicted", static_cast<double>(s.evicted));
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
    if (!ev::isFunction(produce)) return ev::throwTypeError("bro.tiles.createCache: `produce(tile)` is a function");

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
    cfg.maxHeight = std::max(0.0, numberOr(opts, "maxHeight", 0.0));
    cfg.pixelsPerCell = numberOr(opts, "pixelsPerCell", 2.0);
    if (!(cfg.pixelsPerCell > 0)) return ev::throwRangeError("bro.tiles.createCache: pixelsPerCell is positive");

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

    // `produce` lives on the object, so the collector traces it with the cache.
    const Rooted obj(g_cacheClass.createInstance(std::move(cache)));
    ev::setProperty(obj, "produce", produce);
    return obj;
}

} // namespace

void installTiles() {
    g_cacheClass.install("TileCache", 0, nullptr, decorateCache, /*global=*/false);
    const auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    const Rooted broObj(bro.value);
    ev::ObjectBuilder tiles;
    tiles.def("createCache", 1, js_createCache);
    ev::setProperty(broObj, "tiles", tiles.get());
}

} // namespace bro::bronze_host
