#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/terrain_manager.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

HostClass g_terrainClass;

namespace {

std::unordered_set<HostTerrainCell*>& allTerrainInstances() {
    static std::unordered_set<HostTerrainCell*> s;
    return s;
}

scene::TerrainConfig parseTerrainConfig(Value opts) {
    scene::TerrainConfig cfg;
    if (!ev::isObject(opts)) return cfg;

    ev::Persistent root(opts);
    Value cs = ev::getProperty(root.get(), "chunkSize");
    if (ev::isObject(cs)) {
        Value cx = ev::getElement(cs, 0);
        Value cy = ev::getElement(cs, 1);
        Value cz = ev::getElement(cs, 2);
        if (ev::isNumber(cx)) cfg.chunkSizeX = static_cast<int>(ev::toDouble(cx));
        if (ev::isNumber(cy)) cfg.chunkSizeY = static_cast<int>(ev::toDouble(cy));
        if (ev::isNumber(cz)) cfg.chunkSizeZ = static_cast<int>(ev::toDouble(cz));
    }

    auto numProp = [&](const char* name, double def) -> double {
        Value v = ev::getProperty(root.get(), name);
        return ev::isNumber(v) ? ev::toDouble(v) : def;
    };

    cfg.cellSize = static_cast<float>(numProp("cellSize", cfg.cellSize));
    cfg.loadRadius = static_cast<int>(numProp("loadRadius", cfg.loadRadius));
    cfg.unloadRadius = static_cast<int>(numProp("unloadRadius", cfg.unloadRadius));
    cfg.maxLoadsPerUpdate = static_cast<int>(numProp("maxLoadsPerUpdate", cfg.maxLoadsPerUpdate));
    cfg.seed = static_cast<int>(numProp("seed", cfg.seed));
    cfg.baseHeight = static_cast<int>(numProp("baseHeight", cfg.baseHeight));
    cfg.heightAmplitude = static_cast<int>(numProp("heightAmplitude", cfg.heightAmplitude));
    cfg.seaLevel = static_cast<int>(numProp("seaLevel", cfg.seaLevel));
    cfg.meshMode = static_cast<int>(numProp("meshMode", cfg.meshMode));
    cfg.terraceStep = static_cast<float>(numProp("terraceStep", cfg.terraceStep));
    cfg.continentFrequency = static_cast<float>(numProp("continentFrequency", cfg.continentFrequency));
    cfg.continentMin = static_cast<float>(numProp("continentMin", cfg.continentMin));
    cfg.continentMax = static_cast<float>(numProp("continentMax", cfg.continentMax));
    cfg.mountainFrequency = static_cast<float>(numProp("mountainFrequency", cfg.mountainFrequency));
    cfg.mountainAmplitude = static_cast<float>(numProp("mountainAmplitude", cfg.mountainAmplitude));
    cfg.mountainOctaves = static_cast<int>(numProp("mountainOctaves", cfg.mountainOctaves));
    cfg.lodLevelCount = static_cast<int>(numProp("lodLevels", cfg.lodLevelCount));
    cfg.lodScaleFactor = static_cast<int>(numProp("lodScaleFactor", cfg.lodScaleFactor));
    cfg.planetRadius = static_cast<float>(numProp("planetRadius", cfg.planetRadius));

    Value orig = ev::getProperty(root.get(), "origin");
    if (ev::isObject(orig)) {
        Value ox = ev::getElement(orig, 0);
        Value oy = ev::getElement(orig, 1);
        Value oz = ev::getElement(orig, 2);
        if (ev::isNumber(ox)) cfg.origin.x = static_cast<float>(ev::toDouble(ox));
        if (ev::isNumber(oy)) cfg.origin.y = static_cast<float>(ev::toDouble(oy));
        if (ev::isNumber(oz)) cfg.origin.z = static_cast<float>(ev::toDouble(oz));
    }

    Value noise = ev::getProperty(root.get(), "noise");
    if (ev::isObject(noise)) {
        ev::Persistent np(noise);
        Value f = ev::getProperty(np.get(), "frequency");
        if (ev::isNumber(f)) cfg.noiseFrequency = static_cast<float>(ev::toDouble(f));
        Value o = ev::getProperty(np.get(), "octaves");
        if (ev::isNumber(o)) cfg.noiseOctaves = static_cast<int>(ev::toDouble(o));
        Value g = ev::getProperty(np.get(), "gain");
        if (ev::isNumber(g)) cfg.noiseGain = static_cast<float>(ev::toDouble(g));
        Value l = ev::getProperty(np.get(), "lacunarity");
        if (ev::isNumber(l)) cfg.noiseLacunarity = static_cast<float>(ev::toDouble(l));
    }

    Value pal = ev::getProperty(root.get(), "palette");
    if (!ev::isUndefined(pal) && !ev::isNull(pal)) {
        std::vector<float> pvec;
        if (readFloatVector(pal, pvec)) {
            cfg.palette = std::move(pvec);
        }
    }

    return cfg;
}

bool readVec3ArrayOrObj(Value v, bromath::Vec3& out) {
    if (!ev::isObject(v)) return false;
    ev::Persistent p(v);
    Value vx = ev::getProperty(p.get(), "x");
    if (ev::isNumber(vx)) {
        out.x = static_cast<float>(ev::toDouble(vx));
        Value vy = ev::getProperty(p.get(), "y");
        out.y = ev::isNumber(vy) ? static_cast<float>(ev::toDouble(vy)) : 0.0f;
        Value vz = ev::getProperty(p.get(), "z");
        out.z = ev::isNumber(vz) ? static_cast<float>(ev::toDouble(vz)) : 0.0f;
        return true;
    }
    Value e0 = ev::getElement(p.get(), 0);
    Value e1 = ev::getElement(p.get(), 1);
    Value e2 = ev::getElement(p.get(), 2);
    if (ev::isNumber(e0)) {
        out.x = static_cast<float>(ev::toDouble(e0));
        out.y = ev::isNumber(e1) ? static_cast<float>(ev::toDouble(e1)) : 0.0f;
        out.z = ev::isNumber(e2) ? static_cast<float>(ev::toDouble(e2)) : 0.0f;
        return true;
    }
    return false;
}

}  // namespace

bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY) {
    if (!handle) return false;
    auto* cell = static_cast<HostTerrainCell*>(handle);
    if (allTerrainInstances().find(cell) == allTerrainInstances().end() || !cell->manager) {
        return false;
    }
    const bromath::Vec3 origin{x, rayStartY, z};
    const bromath::Vec3 dir{0.0f, -1.0f, 0.0f};
    auto hit = cell->manager->raycast(origin, dir, rayLength);
    if (!hit.hit) return false;
    outY = hit.worldPos[1];
    return true;
}

Value wrapTerrain(std::unique_ptr<scene::TerrainManager> mgr) {
    ensureSceneClassesInstalled();
    auto* cell = new HostTerrainCell();
    cell->manager = std::move(mgr);
    allTerrainInstances().insert(cell);

    Value inst = g_terrainClass.make(cell, [](void* p) {
        auto* c = static_cast<HostTerrainCell*>(p);
        allTerrainInstances().erase(c);
        delete c;
    });
    return inst;
}

void installSceneGraphTerrain(ObjectBuilder& b) {
    b.def("createTerrain", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::null();
        Value opts = !a.empty() ? a[0] : ev::undefined();
        scene::TerrainConfig cfg = parseTerrainConfig(opts);
        auto mgr = std::make_unique<scene::TerrainManager>(*g);
        mgr->configure(cfg);
        return wrapTerrain(std::move(mgr));
    });
}

void ensureTerrainClassInstalled() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    g_terrainClass.install("Terrain", 0, nullptr, [](ObjectBuilder& b) {
        b.def("update", 3, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager || a.size() < 3) return ev::fromDouble(0);
            float x = static_cast<float>(ev::toDouble(a[0]));
            float y = static_cast<float>(ev::toDouble(a[1]));
            float z = static_cast<float>(ev::toDouble(a[2]));
            return ev::fromDouble(c->manager->update(x, y, z));
        });

        b.def("raycast", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager || a.size() < 2) return ev::null();
            bromath::Vec3 origin{0, 0, 0};
            bromath::Vec3 dir{0, -1, 0};
            if (!readVec3ArrayOrObj(a[0], origin) || !readVec3ArrayOrObj(a[1], dir)) {
                return ev::null();
            }
            float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 1000.0f;
            auto hit = c->manager->raycast(origin, dir, maxDist);
            if (!hit.hit) return ev::null();

            ObjectBuilder res;
            res.set("hit", ev::fromBool(true));
            res.set("distance", ev::fromDouble(hit.distance));

            Value posVal = hostArrayOf(3, [&](size_t i) {
                return ev::fromDouble(hit.worldPos[i]);
            });
            res.set("position", posVal);

            Value normVal = hostArrayOf(3, [&](size_t i) {
                return ev::fromDouble(hit.normal[i]);
            });
            res.set("normal", normVal);

            Value chunkVal = hostArrayOf(2, [&](size_t i) {
                return ev::fromDouble(i == 0 ? hit.chunk.x : hit.chunk.z);
            });
            res.set("chunk", chunkVal);

            Value voxVal = hostArrayOf(3, [&](size_t i) {
                int val = (i == 0) ? hit.localX : ((i == 1) ? hit.localY : hit.localZ);
                return ev::fromDouble(val);
            });
            res.set("voxel", voxVal);

            res.set("material", ev::fromDouble(hit.material));
            return res.get();
        });

        b.def("setVoxel", 4, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager || a.size() < 4) return ev::fromBool(false);
            float wx = static_cast<float>(ev::toDouble(a[0]));
            float wy = static_cast<float>(ev::toDouble(a[1]));
            float wz = static_cast<float>(ev::toDouble(a[2]));
            uint8_t mat = static_cast<uint8_t>(ev::toDouble(a[3]));
            return ev::fromBool(c->manager->setVoxel(wx, wy, wz, mat));
        });

        b.def("getVoxel", 3, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager || a.size() < 3) return ev::fromDouble(0);
            float wx = static_cast<float>(ev::toDouble(a[0]));
            float wy = static_cast<float>(ev::toDouble(a[1]));
            float wz = static_cast<float>(ev::toDouble(a[2]));
            return ev::fromDouble(c->manager->getVoxel(wx, wy, wz));
        });

        b.def("rebuild", 0, [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            if (c && c->manager) c->manager->rebuildDirty();
            return ev::undefined();
        });

        b.def("configure", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (c && c->manager && !a.empty()) {
                c->manager->configure(parseTerrainConfig(a[0]));
            }
            return ev::undefined();
        });

        b.def("invalidateRegion", 4, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager) return ev::undefined();
            if (a.size() < 4) return ev::throwTypeError("invalidateRegion(x0, z0, x1, z1) needs 4 numbers");
            float x0 = static_cast<float>(ev::toDouble(a[0]));
            float z0 = static_cast<float>(ev::toDouble(a[1]));
            float x1 = static_cast<float>(ev::toDouble(a[2]));
            float z1 = static_cast<float>(ev::toDouble(a[3]));
            c->manager->invalidateRegion(x0, z0, x1, z1);
            return ev::undefined();
        });

        b.def("destroy", 0, [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            if (c && c->manager) {
                c->manager->clear();
                c->manager.reset();
            }
            return ev::undefined();
        });

        b.accessor("chunkCount", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            return ev::fromDouble((c && c->manager) ? c->manager->chunkCount() : 0);
        }, nullptr);

        b.accessor("triangleCount", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            return ev::fromDouble((c && c->manager) ? c->manager->totalTriangles() : 0);
        }, nullptr);

        b.accessor("vertexCount", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            return ev::fromDouble((c && c->manager) ? c->manager->totalVertices() : 0);
        }, nullptr);

        b.accessor("farDistance", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            return ev::fromDouble((c && c->manager) ? c->manager->farDistance() : 1000.0);
        }, nullptr);

        b.accessor("planetRadius", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            return ev::fromDouble((c && c->manager) ? c->manager->config().planetRadius : 0.0);
        }, nullptr);

        b.accessor("origin", [](Value self_, std::span<const Value>) -> Value {
            auto* c = terrainCellOf(self_);
            if (!c || !c->manager) return ev::null();
            auto& o = c->manager->config().origin;
            return hostArrayOf(3, [&](size_t i) {
                return ev::fromDouble(i == 0 ? o.x : (i == 1 ? o.y : o.z));
            });
        }, nullptr);
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
