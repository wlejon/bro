#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "bronze_host/host_mesh_internal.h"
#include "bronze_host/host_ai_internal.h"
#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "util/asset_path.h"
#include "broimage/decode.h"
#include "tile/pathfind.h"
#include "tile/region.h"
#include "tile/coord.h"
#include <brogameagent/nav_grid.h>
#include <brogameagent/types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

static void readFloatArray(Value v, std::vector<float>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return;
    auto info = ev::typedArrayInfo(v);
    if (info && info.data && info.byteLength > 0) {
        if (info.elementKind == ev::elements::Float32) {
            const float* fp = reinterpret_cast<const float*>(info.data);
            out.assign(fp, fp + info.elementCount);
            return;
        }
    }
    auto abInfo = ev::arrayBufferInfo(v);
    if (abInfo && abInfo.data && abInfo.byteLength > 0) {
        size_t count = abInfo.byteLength / sizeof(float);
        const float* fp = reinterpret_cast<const float*>(abInfo.data);
        out.assign(fp, fp + count);
        return;
    }
    if (ev::isObject(v)) {
        Value lenVal = ev::getProperty(v, "length");
        if (ev::isNumber(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            out.resize(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(v, i);
                out[i] = ev::isNumber(el) ? static_cast<float>(ev::toDouble(el)) : 0.0f;
            }
        }
    }
}

static void readColor4(Value obj, const char* prop, float out[4]) {
    if (!ev::isObject(obj)) return;
    Value v = ev::getProperty(obj, prop);
    if (ev::isObject(v)) {
        Value lenVal = ev::getProperty(v, "length");
        if (ev::isNumber(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            for (uint32_t i = 0; i < 4 && i < len; ++i) {
                Value el = ev::getElement(v, i);
                out[i] = ev::isNumber(el) ? static_cast<float>(ev::toDouble(el)) : (i == 3 ? 1.0f : 0.0f);
            }
        }
    }
}

static int stampNavGrid(scene::TileWorld* world, brogameagent::NavGrid* ng,
                        uint32_t blockMask, float padding) {
    const auto& cfg = world->config();
    const float cs = cfg.cellSize;
    int blocked = 0;
    for (int y = 0; y < cfg.height; ++y) {
        for (int x = 0; x < cfg.width; ++x) {
            if (world->isWalkable(x, y, blockMask)) continue;
            brogameagent::AABB box;
            world->cellCenterWorldXZ(x, y, box.cx, box.cz);
            box.hw = cs * 0.49f;
            box.hd = cs * 0.49f;
            ng->addObstacle(box, padding);
            ++blocked;
        }
    }
    return blocked;
}

static tile::PassFn makePassFn(uint32_t blockMask) {
    return [blockMask](const tile::TileGrid& g, tile::Cell c) {
        return g.tile(0, c) != 0 && (g.flags(c) & blockMask) == 0;
    };
}

static tile::CostFn makeCostFn(std::vector<float> costs) {
    if (costs.empty()) return nullptr;
    return [costs = std::move(costs)](const tile::TileGrid& g, tile::Cell,
                                      tile::Cell to) -> float {
        uint16_t id = g.tile(0, to);
        float c = (id < costs.size()) ? costs[id] : 1.0f;
        return c < 1.0f ? 1.0f : c;
    };
}

static tile::Conn parseConn(Value v, tile::Conn def = tile::Conn::Edge) {
    tile::Conn conn = def;
    if (ev::isString(v)) {
        std::string s = ev::toUtf8(v);
        if (s == "vertex") conn = tile::Conn::Vertex;
    }
    return conn;
}

struct SearchOpts {
    uint32_t blockMask = 0;
    std::vector<float> costs;
    tile::Conn conn = tile::Conn::Edge;
};

static SearchOpts parseSearchOpts(Value v) {
    SearchOpts o;
    if (!ev::isObject(v)) return o;
    Value mv = ev::getProperty(v, "blockMask");
    if (ev::isNumber(mv)) o.blockMask = static_cast<uint32_t>(ev::toDouble(mv));
    Value costs = ev::getProperty(v, "costs");
    readFloatArray(costs, o.costs);
    Value conn = ev::getProperty(v, "conn");
    o.conn = parseConn(conn);
    return o;
}

static Value makeCellObj(tile::Cell c) {
    ObjectBuilder ob;
    ob.set("x", ev::fromDouble(c.x));
    ob.set("y", ev::fromDouble(c.y));
    return ob.get();
}

static Value makeCellArray(const std::vector<tile::Cell>& cells) {
    return hostArrayOf(cells.size(), [&](size_t i) {
        return makeCellObj(cells[i]);
    });
}

static bool readCellArg(Value v, tile::Cell& out) {
    if (!ev::isObject(v)) return false;
    Value lenVal = ev::getProperty(v, "length");
    if (ev::isNumber(lenVal)) {
        Value jx = ev::getElement(v, 0);
        Value jy = ev::getElement(v, 1);
        if (ev::isNumber(jx) && ev::isNumber(jy)) {
            out.x = static_cast<int>(ev::toDouble(jx));
            out.y = static_cast<int>(ev::toDouble(jy));
            return true;
        }
    }
    Value jx = ev::getProperty(v, "x");
    Value jy = ev::getProperty(v, "y");
    if (ev::isNumber(jx) && ev::isNumber(jy)) {
        out.x = static_cast<int>(ev::toDouble(jx));
        out.y = static_cast<int>(ev::toDouble(jy));
        return true;
    }
    return false;
}

static tile::MatchFn makeMatchFn(Value v, const tile::TileGrid& g,
                                 const tile::Cell* seed, int& layerOut, tile::Conn& connOut) {
    int layer = 0;
    double flag = 0;
    double id = -1;
    if (ev::isObject(v)) {
        Value lv = ev::getProperty(v, "layer");
        if (ev::isNumber(lv)) layer = static_cast<int>(ev::toDouble(lv));
        Value fv = ev::getProperty(v, "flag");
        if (ev::isNumber(fv)) flag = ev::toDouble(fv);
        Value iv = ev::getProperty(v, "id");
        if (ev::isNumber(iv)) id = ev::toDouble(iv);
        Value conn = ev::getProperty(v, "conn");
        connOut = parseConn(conn);
    }
    layerOut = layer;
    if (flag > 0) return tile::matchFlag(static_cast<uint32_t>(flag));
    if (id >= 0) return tile::matchTile(layer, static_cast<uint16_t>(id));
    if (seed)    return tile::matchTile(layer, g.tile(layer, *seed));
    return [layer](const tile::TileGrid& gg, tile::Cell c) { return gg.tile(layer, c) != 0; };
}

static void filterInBounds(const tile::TileGrid& g, std::vector<tile::Cell>& cells) {
    std::erase_if(cells, [&g](tile::Cell c) { return !g.inBounds(c); });
}

} // namespace

void installTileWorldExtra(ObjectBuilder& b) {
    b.def("addObjectKind", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || a.empty()) return ev::fromDouble(-1);
        bromesh::MeshData* md = hostMeshDataOf(a[0]);
        if (!md) return ev::throwTypeError("addObjectKind: first argument must be a mesh");

        scene::TileWorld::ObjectStyle style;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value s = a[1];
            readColor4(s, "color", style.color);
            Value roughVal = ev::getProperty(s, "roughness");
            if (ev::isNumber(roughVal)) style.roughness = static_cast<float>(ev::toDouble(roughVal));
            Value metVal = ev::getProperty(s, "metallic");
            if (ev::isNumber(metVal)) style.metallic = static_cast<float>(ev::toDouble(metVal));
            Value dsVal = ev::getProperty(s, "doubleSided");
            if (!ev::isUndefined(dsVal)) style.doubleSided = ev::toBool(dsVal);
            Value acVal = ev::getProperty(s, "alphaCutoff");
            if (ev::isNumber(acVal)) style.alphaCutoff = static_cast<float>(ev::toDouble(acVal));
            Value csVal = ev::getProperty(s, "castsShadow");
            if (!ev::isUndefined(csVal)) style.castsShadow = ev::toBool(csVal);
            Value colsVal = ev::getProperty(s, "atlasColumns");
            if (ev::isNumber(colsVal)) style.atlasCols = static_cast<int>(ev::toDouble(colsVal));
            Value rowsVal = ev::getProperty(s, "atlasRows");
            if (ev::isNumber(rowsVal)) style.atlasRows = static_cast<int>(ev::toDouble(rowsVal));

            Value tex = ev::getProperty(s, "texture");
            if (ev::isString(tex)) {
                std::string p = ev::toUtf8(tex);
                broimage::Image img;
                std::string err;
                if (broimage::decode_file(util::resolveAssetPath(p), img, &err) &&
                    img.width > 0 && img.height > 0) {
                    style.texWidth = img.width;
                    style.texHeight = img.height;
                    style.texPixels = std::move(img.pixels);
                }
            }
        }

        int kind = w->addObjectKind(bromesh::MeshData(*md), style);
        return ev::fromDouble(kind);
    });

    b.def("addObject", 4, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || a.size() < 3) return ev::fromDouble(-1);
        scene::TileWorld::ObjectPlacement p;
        if (a.size() > 3 && ev::isObject(a[3])) {
            Value o = a[3];
            Value yv = ev::getProperty(o, "yaw");
            if (ev::isNumber(yv)) p.yaw = static_cast<float>(ev::toDouble(yv));
            Value sv = ev::getProperty(o, "scale");
            if (ev::isNumber(sv)) p.scale = static_cast<float>(ev::toDouble(sv));
            Value yoff = ev::getProperty(o, "yOffset");
            if (ev::isNumber(yoff)) p.yOffset = static_cast<float>(ev::toDouble(yoff));
            Value xoff = ev::getProperty(o, "offsetX");
            if (ev::isNumber(xoff)) p.offsetX = static_cast<float>(ev::toDouble(xoff));
            Value zoff = ev::getProperty(o, "offsetZ");
            if (ev::isNumber(zoff)) p.offsetZ = static_cast<float>(ev::toDouble(zoff));
            Value var = ev::getProperty(o, "variant");
            if (ev::isNumber(var)) p.variant = static_cast<int>(ev::toDouble(var));
            readColor4(o, "color", p.color);
        }
        int kind = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int x = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int y = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        int idx = w->addObject(kind, x, y, p);
        return ev::fromDouble(idx);
    });

    b.def("clearObjects", 1, [](Value self_, std::span<const Value> a) {
        auto* w = tileWorldOf(self_);
        if (w) {
            int kind = (!a.empty() && ev::isNumber(a[0])) ? static_cast<int>(ev::toDouble(a[0])) : -1;
            w->clearObjects(kind);
        }
        return ev::undefined();
    });

    b.def("objectCount", 1, [](Value self_, std::span<const Value> a) {
        auto* w = tileWorldOf(self_);
        if (!w || a.empty()) return ev::fromDouble(0);
        int kind = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        return ev::fromDouble(w->objectCount(kind));
    });

    b.def("rebuildObjects", 0, [](Value self_, std::span<const Value>) {
        auto* w = tileWorldOf(self_);
        if (w) w->rebuildObjects();
        return ev::undefined();
    });

    b.def("syncNavGrid", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || a.empty()) return ev::fromDouble(0);
        HostNavGrid* hng = unwrapNavGrid(a[0]);
        if (!hng || !hng->grid) return ev::throwTypeError("syncNavGrid: first argument must be a nav grid");
        uint32_t mask = 0; float padding = 0.0f;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value mv = ev::getProperty(a[1], "blockMask");
            if (ev::isNumber(mv)) mask = static_cast<uint32_t>(ev::toDouble(mv));
            Value pv = ev::getProperty(a[1], "padding");
            if (ev::isNumber(pv)) padding = static_cast<float>(ev::toDouble(pv));
        }
        return ev::fromDouble(stampNavGrid(w, hng->grid.get(), mask, padding));
    });

    b.def("toNavGrid", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w) return ev::null();
        const auto& cfg = w->config();
        uint32_t mask = 0; float padding = 0.0f;
        if (!a.empty() && ev::isObject(a[0])) {
            Value mv = ev::getProperty(a[0], "blockMask");
            if (ev::isNumber(mv)) mask = static_cast<uint32_t>(ev::toDouble(mv));
            Value pv = ev::getProperty(a[0], "padding");
            if (ev::isNumber(pv)) padding = static_cast<float>(ev::toDouble(pv));
        }
        const float cs = cfg.cellSize;
        float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
        w->worldBounds(minX, minZ, maxX, maxZ);
        auto grid = std::make_unique<brogameagent::NavGrid>(minX, minZ, maxX, maxZ, cs);
        stampNavGrid(w, grid.get(), mask, padding);
        return makeNavGridHandle(std::move(grid));
    });

    b.def("findPath", 5, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 4) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
        SearchOpts o = parseSearchOpts(a.size() > 4 ? a[4] : ev::undefined());
        auto path = tile::aStar(*w->grid(), {x0, y0}, {x1, y1},
                                makePassFn(o.blockMask), makeCostFn(std::move(o.costs)),
                                o.conn);
        return makeCellArray(path);
    });

    b.def("distanceField", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.empty()) return ev::null();

        std::vector<tile::Cell> sources;
        Value srcVal = a[0];
        if (ev::isObject(srcVal)) {
            Value lenVal = ev::getProperty(srcVal, "length");
            if (ev::isNumber(lenVal)) {
                int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
                for (int32_t i = 0; i < len; ++i) {
                    Value el = ev::getElement(srcVal, i);
                    tile::Cell c;
                    if (readCellArg(el, c)) sources.push_back(c);
                }
            } else {
                tile::Cell c;
                if (readCellArg(srcVal, c)) sources.push_back(c);
            }
        }
        if (sources.empty()) return ev::null();

        SearchOpts o = parseSearchOpts(a.size() > 1 ? a[1] : ev::undefined());

        if (!o.costs.empty()) {
            std::vector<float> field = tile::distanceFieldWeighted(
                *w->grid(), sources, makePassFn(o.blockMask),
                makeCostFn(std::move(o.costs)), o.conn);
            ev::Persistent view(ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(field.size())));
            ev::fillTypedArray(view.get(), std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(field.data()), field.size() * sizeof(float)));
            return view.get();
        }

        std::vector<int> field = tile::distanceField(*w->grid(), sources,
                                                     makePassFn(o.blockMask), o.conn);
        ev::Persistent view(ev::createTypedArray(ev::elements::Int32, static_cast<uint32_t>(field.size())));
        ev::fillTypedArray(view.get(), std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(field.data()), field.size() * sizeof(int32_t)));
        return view.get();
    });

    b.def("floodFill", 3, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 2) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        tile::Cell seed{ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0,
                        ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0};
        int layer = 0; tile::Conn conn = tile::Conn::Edge;
        tile::MatchFn match = makeMatchFn(a.size() > 2 ? a[2] : ev::undefined(), g,
                                          &seed, layer, conn);
        return makeCellArray(tile::floodFill(g, seed, match, conn));
    });

    b.def("components", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        int layer = 0; tile::Conn conn = tile::Conn::Edge;
        tile::MatchFn match = makeMatchFn(!a.empty() ? a[0] : ev::undefined(), g,
                                          nullptr, layer, conn);
        auto comps = tile::components(g, match, conn);
        return hostArrayOf(comps.size(), [&](size_t i) {
            return makeCellArray(comps[i]);
        });
    });

    b.def("cellDistance", 5, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 4) return ev::fromDouble(-1);
        int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
        tile::Conn conn = parseConn(a.size() > 4 ? a[4] : ev::undefined());
        return ev::fromDouble(tile::distance(w->grid()->topology(),
                                            {x0, y0}, {x1, y1}, conn));
    });

    b.def("cellNeighbors", 3, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 2) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        tile::Conn conn = parseConn(a.size() > 2 ? a[2] : ev::undefined());
        tile::Neighbors nb = tile::neighbors(g.topology(), {x, y}, conn);
        std::vector<tile::Cell> cells(nb.begin(), nb.end());
        filterInBounds(g, cells);
        return makeCellArray(cells);
    });

    b.def("cellRing", 4, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 3) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int r = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        tile::Conn conn = parseConn(a.size() > 3 ? a[3] : ev::undefined());
        auto cells = tile::ring(g.topology(), {x, y}, r, conn);
        filterInBounds(g, cells);
        return makeCellArray(cells);
    });

    b.def("cellsInRange", 4, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 3) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int r = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        tile::Conn conn = parseConn(a.size() > 3 ? a[3] : ev::undefined());
        auto cells = tile::range(g.topology(), {x, y}, r, conn);
        filterInBounds(g, cells);
        return makeCellArray(cells);
    });

    b.def("cellLine", 4, [](Value self_, std::span<const Value> a) -> Value {
        auto* w = tileWorldOf(self_);
        if (!w || !w->grid() || a.size() < 4) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const tile::TileGrid& g = *w->grid();
        int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
        auto cells = tile::line(g.topology(), {x0, y0}, {x1, y1});
        filterInBounds(g, cells);
        return makeCellArray(cells);
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
