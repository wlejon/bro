// native_scene_tile_ops.cpp — TileWorld graph search, region, and coordinate operations.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "scene/tile_world.h"
#include "tile/pathfind.h"
#include "tile/region.h"
#include "tile/coord.h"
#include "util/log.h"
#include <json.hpp>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace bro::bronze_host {

namespace {

static thread_local std::string tl_tileOpsJson;

static bro::tile::Conn parseConn(const char* connStr) {
    if (connStr && std::string_view(connStr) == "vertex") {
        return bro::tile::Conn::Vertex;
    }
    return bro::tile::Conn::Edge;
}

} // namespace

extern "C" {

const char* bro_tile_world_TileWorld_distanceField(void* self, const char* sourcesJson, const char* jsonOpts) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();

    std::vector<bro::tile::Cell> sources;
    if (sourcesJson && sourcesJson[0]) {
        try {
            auto sj = json::parse(sourcesJson);
            if (sj.is_array()) {
                for (const auto& item : sj) {
                    if (item.contains("x") && item.contains("y")) {
                        sources.push_back({item["x"].get<int>(), item["y"].get<int>()});
                    }
                }
            } else if (sj.is_object() && sj.contains("x") && sj.contains("y")) {
                sources.push_back({sj["x"].get<int>(), sj["y"].get<int>()});
            }
        } catch (const std::exception& e) {
            LOG_WARN("TileWorld.distanceField: failed to parse sourcesJson: %s", e.what());
        } catch (...) {
            LOG_WARN("TileWorld.distanceField: unknown exception while parsing sourcesJson");
        }
    }
    if (sources.empty()) sources.push_back({0, 0});

    uint32_t blockMask = 0;
    bool allowDiag = false;
    std::vector<float> costs;
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = json::parse(jsonOpts);
            if (j.contains("blockMask") && j["blockMask"].is_number()) blockMask = j["blockMask"].get<uint32_t>();
            if (j.contains("allowDiagonal") && j["allowDiagonal"].is_boolean()) allowDiag = j["allowDiagonal"].get<bool>();
            if (j.contains("costs") && j["costs"].is_array()) {
                for (const auto& item : j["costs"]) {
                    costs.push_back(item.is_number() ? item.get<float>() : 1.0f);
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("TileWorld.distanceField: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("TileWorld.distanceField: unknown exception while parsing jsonOpts");
        }
    }

    bro::tile::Conn conn = allowDiag ? bro::tile::Conn::Vertex : bro::tile::Conn::Edge;
    bro::tile::PassFn pass = [blockMask](const bro::tile::TileGrid& g, bro::tile::Cell cell) {
        if (g.tile(0, cell) == 0) return false;
        if (blockMask != 0 && (g.flags(cell) & blockMask) != 0) return false;
        return true;
    };

    std::ostringstream ss;
    ss << "[";
    if (!costs.empty()) {
        bro::tile::CostFn costFn = [&costs](const bro::tile::TileGrid& g, bro::tile::Cell, bro::tile::Cell to) -> float {
            uint16_t tid = g.tile(0, to);
            return (tid < costs.size()) ? costs[tid] : 1.0f;
        };
        auto field = bro::tile::distanceFieldWeighted(grid, sources, pass, costFn, conn);
        for (size_t i = 0; i < field.size(); ++i) {
            if (i > 0) ss << ",";
            ss << field[i];
        }
    } else {
        auto field = bro::tile::distanceField(grid, sources, pass, conn);
        for (size_t i = 0; i < field.size(); ++i) {
            if (i > 0) ss << ",";
            ss << field[i];
        }
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

const char* bro_tile_world_TileWorld_floodFill(void* self, int32_t seedX, int32_t seedY, const char* jsonOpts) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();

    bro::tile::Cell seed{seedX, seedY};
    bro::tile::MatchFn match;
    bro::tile::Conn conn = bro::tile::Conn::Edge;
    int layer = 0;

    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = json::parse(jsonOpts);
            if (j.contains("conn") && j["conn"].is_string()) {
                conn = parseConn(j["conn"].get<std::string>().c_str());
            }
            if (j.contains("layer") && j["layer"].is_number()) {
                layer = j["layer"].get<int>();
            }
            if (j.contains("flag") && j["flag"].is_number()) {
                match = bro::tile::matchFlag(j["flag"].get<uint32_t>());
            } else if (j.contains("id") && j["id"].is_number()) {
                match = bro::tile::matchTile(layer, j["id"].get<uint16_t>());
            }
        } catch (const std::exception& e) {
            LOG_WARN("TileWorld.floodFill: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("TileWorld.floodFill: unknown exception while parsing jsonOpts");
        }
    }
    if (!match) {
        uint16_t seedId = grid.inBounds(seed) ? grid.tile(layer, seed) : 0;
        match = bro::tile::matchSameTile(layer, seedId);
    }

    auto cells = bro::tile::floodFill(grid, seed, match, conn);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"x\":" << cells[i].x << ",\"y\":" << cells[i].y << "}";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

const char* bro_tile_world_TileWorld_components(void* self, const char* jsonOpts) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();

    bro::tile::MatchFn match;
    bro::tile::Conn conn = bro::tile::Conn::Edge;
    int layer = 0;

    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = json::parse(jsonOpts);
            if (j.contains("conn") && j["conn"].is_string()) {
                conn = parseConn(j["conn"].get<std::string>().c_str());
            }
            if (j.contains("layer") && j["layer"].is_number()) {
                layer = j["layer"].get<int>();
            }
            if (j.contains("flag") && j["flag"].is_number()) {
                match = bro::tile::matchFlag(j["flag"].get<uint32_t>());
            } else if (j.contains("id") && j["id"].is_number()) {
                match = bro::tile::matchTile(layer, j["id"].get<uint16_t>());
            }
        } catch (const std::exception& e) {
            LOG_WARN("TileWorld.components: failed to parse jsonOpts: %s", e.what());
        } catch (...) {
            LOG_WARN("TileWorld.components: unknown exception while parsing jsonOpts");
        }
    }
    if (!match) {
        match = [](const bro::tile::TileGrid& g, bro::tile::Cell cell) {
            return g.tile(0, cell) != 0;
        };
    }

    auto comps = bro::tile::components(grid, match, conn);
    std::ostringstream ss;
    ss << "[";
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        if (ci > 0) ss << ",";
        ss << "[";
        for (size_t i = 0; i < comps[ci].size(); ++i) {
            if (i > 0) ss << ",";
            ss << "{\"x\":" << comps[ci][i].x << ",\"y\":" << comps[ci][i].y << "}";
        }
        ss << "]";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

int32_t bro_tile_world_TileWorld_cellDistance(void* self, int32_t ax, int32_t ay, int32_t bx, int32_t by, const char* connStr) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return 0;
    const auto& grid = *c->tileWorld()->grid();
    return bro::tile::distance(grid.topology(), {ax, ay}, {bx, by}, parseConn(connStr));
}

const char* bro_tile_world_TileWorld_cellRing(void* self, int32_t cx, int32_t cy, int32_t radius, const char* connStr) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();
    auto cells = bro::tile::ring(grid.topology(), {cx, cy}, radius, parseConn(connStr));
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& cell : cells) {
        if (!grid.inBounds(cell)) continue;
        if (!first) ss << ",";
        first = false;
        ss << "{\"x\":" << cell.x << ",\"y\":" << cell.y << "}";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

const char* bro_tile_world_TileWorld_cellsInRange(void* self, int32_t cx, int32_t cy, int32_t radius, const char* connStr) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();
    auto cells = bro::tile::range(grid.topology(), {cx, cy}, radius, parseConn(connStr));
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& cell : cells) {
        if (!grid.inBounds(cell)) continue;
        if (!first) ss << ",";
        first = false;
        ss << "{\"x\":" << cell.x << ",\"y\":" << cell.y << "}";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

const char* bro_tile_world_TileWorld_cellLine(void* self, int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();
    auto cells = bro::tile::line(grid.topology(), {ax, ay}, {bx, by});
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"x\":" << cells[i].x << ",\"y\":" << cells[i].y << "}";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

const char* bro_tile_world_TileWorld_cellNeighbors(void* self, int32_t cx, int32_t cy, const char* connStr) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return "[]";
    const auto& grid = *c->tileWorld()->grid();
    auto nb = bro::tile::neighbors(grid.topology(), {cx, cy}, parseConn(connStr));
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (int i = 0; i < nb.count; ++i) {
        if (!grid.inBounds(nb.items[i])) continue;
        if (!first) ss << ",";
        first = false;
        ss << "{\"x\":" << nb.items[i].x << ",\"y\":" << nb.items[i].y << "}";
    }
    ss << "]";
    tl_tileOpsJson = ss.str();
    return tl_tileOpsJson.c_str();
}

} // extern "C"

bool registerTileWorldOpsNatives(std::string* error) {
    using namespace natives;
    return fn("__bro_native.tile_world.TileWorld_distanceField", (void*)&bro_tile_world_TileWorld_distanceField, "str", {"__bro_native.tile_world.TileWorld", "str", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_floodFill", (void*)&bro_tile_world_TileWorld_floodFill, "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_components", (void*)&bro_tile_world_TileWorld_components, "str", {"__bro_native.tile_world.TileWorld", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_cellDistance", (void*)&bro_tile_world_TileWorld_cellDistance, "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_cellRing", (void*)&bro_tile_world_TileWorld_cellRing, "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_cellsInRange", (void*)&bro_tile_world_TileWorld_cellsInRange, "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_cellLine", (void*)&bro_tile_world_TileWorld_cellLine, "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32"}, error) &&
           fn("__bro_native.tile_world.TileWorld_cellNeighbors", (void*)&bro_tile_world_TileWorld_cellNeighbors, "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "str"}, error);
}

} // namespace bro::bronze_host
