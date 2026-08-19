// AINavGrid implementation for bronze_host.
// 2D grid navigation, A* pathfinding, line-of-sight, obstacle rasterization.

#include "bronze_host/host_ai_internal.h"

namespace bro::bronze_host {

void decorateNavGridProto(ObjectBuilder& b) {
    b.def("isWalkable", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::fromBool(false);
        float x = 0.0f, z = 0.0f;
        if (a.size() >= 2) {
            x = static_cast<float>(numAt(a, 0));
            z = static_cast<float>(numAt(a, 1));
        } else if (!a.empty() && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            x = p.x; z = p.y;
        }
        return ev::fromBool(h->grid->isWalkable(x, z));
    });

    b.def("setWalkable", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::undefined();
        if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            bool walkable = boolAt(a, 2);
            h->grid->setWalkable(x, z, walkable);
        } else if (a.size() >= 2 && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            bool walkable = boolAt(a, 1);
            h->grid->setWalkable(p.x, p.y, walkable);
        }
        return ev::undefined();
    });

    b.def("setCellCost", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::undefined();
        if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            float cost = static_cast<float>(numAt(a, 2));
            h->grid->setCellCost(x, z, cost);
        } else if (a.size() >= 2 && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            float cost = static_cast<float>(numAt(a, 1));
            h->grid->setCellCost(p.x, p.y, cost);
        }
        return ev::undefined();
    });

    b.def("addObstacle", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid || a.empty()) return ev::undefined();
        float padding = (a.size() >= 2) ? static_cast<float>(numAt(a, 1)) : 0.0f;
        h->grid->addObstacle(parseAABB(a[0]), padding);
        return ev::undefined();
    });

    b.def("removeObstacle", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid || a.empty()) return ev::undefined();
        auto box = parseAABB(a[0]);
        float padding = (a.size() >= 2) ? static_cast<float>(numAt(a, 1)) : 0.0f;
        float minX = box.cx - box.hw - padding;
        float maxX = box.cx + box.hw + padding;
        float minZ = box.cz - box.hd - padding;
        float maxZ = box.cz + box.hd + padding;
        float cs = h->grid->cellSize();
        if (cs > 0.0f) {
            for (float x = minX; x <= maxX; x += cs * 0.5f) {
                for (float z = minZ; z <= maxZ; z += cs * 0.5f) {
                    h->grid->setWalkable(x, z, true);
                }
            }
        }
        return ev::undefined();
    });

    b.def("findPath", 5, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        float fx = 0.0f, fz = 0.0f, tx = 0.0f, tz = 0.0f;
        bool requireFull = false;

        if (a.size() >= 4 && !ev::isObject(a[0]) && !ev::isObject(a[1])) {
            fx = static_cast<float>(numAt(a, 0));
            fz = static_cast<float>(numAt(a, 1));
            tx = static_cast<float>(numAt(a, 2));
            tz = static_cast<float>(numAt(a, 3));
            if (a.size() >= 5 && ev::isObject(a[4])) {
                requireFull = getBoolProperty(a[4], "requireFullPath", false);
            }
        } else if (a.size() >= 2) {
            auto pFrom = parseVec2(a[0]);
            auto pTo   = parseVec2(a[1]);
            fx = pFrom.x; fz = pFrom.y;
            tx = pTo.x;   tz = pTo.y;
            if (a.size() >= 3 && ev::isObject(a[2])) {
                requireFull = getBoolProperty(a[2], "requireFullPath", false);
            }
        }

        auto res = h->grid->findPathEx({fx, fz}, {tx, tz}, requireFull);

        Value arr = hostArrayOf(res.points.size(), [&](size_t i) {
            return makeVec2Value(res.points[i].x, res.points[i].y);
        });
        ev::Persistent p(arr);
        p.set(ev::setProperty(p.get(), "partial", ev::fromBool(res.partial)));
        return p.get();
    });

    b.def("hasLineOfSight", 4, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::fromBool(false);
        float fx = 0.0f, fz = 0.0f, tx = 0.0f, tz = 0.0f;
        if (a.size() >= 4) {
            fx = static_cast<float>(numAt(a, 0));
            fz = static_cast<float>(numAt(a, 1));
            tx = static_cast<float>(numAt(a, 2));
            tz = static_cast<float>(numAt(a, 3));
        } else if (a.size() >= 2) {
            auto pFrom = parseVec2(a[0]);
            auto pTo   = parseVec2(a[1]);
            fx = pFrom.x; fz = pFrom.y;
            tx = pTo.x;   tz = pTo.y;
        }
        return ev::fromBool(h->grid->hasGridLOS({fx, fz}, {tx, tz}));
    });

    b.def("raycast", 4, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::null();
        float fx = 0.0f, fz = 0.0f, tx = 0.0f, tz = 0.0f;
        if (a.size() >= 4) {
            fx = static_cast<float>(numAt(a, 0));
            fz = static_cast<float>(numAt(a, 1));
            tx = static_cast<float>(numAt(a, 2));
            tz = static_cast<float>(numAt(a, 3));
        } else if (a.size() >= 2) {
            auto pFrom = parseVec2(a[0]);
            auto pTo   = parseVec2(a[1]);
            fx = pFrom.x; fz = pFrom.y;
            tx = pTo.x;   tz = pTo.y;
        }
        bool clear = h->grid->hasGridLOS({fx, fz}, {tx, tz});
        ObjectBuilder res;
        res.set("hit", ev::fromBool(!clear));
        res.set("clear", ev::fromBool(clear));
        return res.get();
    });

    b.accessor("width", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->width()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("height", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->height()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("cellSize", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->cellSize()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("minX", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->minX()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("minZ", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->minZ()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("maxX", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->maxX()) : ev::fromDouble(0);
    }, nullptr);

    b.accessor("maxZ", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->maxZ()) : ev::fromDouble(0);
    }, nullptr);
}

Value makeNavGridHandle(std::unique_ptr<brogameagent::NavGrid> grid) {
    auto* h = new HostNavGrid();
    h->grid = std::move(grid);

    ObjectBuilder b(g_navGridClass.make(h, [](void* p) {
        delete static_cast<HostNavGrid*>(p);
    }));

    return b.get();
}

Value aiCreateNavGrid(Value, std::span<const Value> a) {
    Value opts = a.empty() ? ev::undefined() : a[0];
    if (!ev::isObject(opts)) {
        return ev::throwTypeError("createNavGrid(options) requires an options object");
    }

    ev::Persistent root(opts);
    double cellSize = getDoubleProperty(root.get(), "cellSize", 0.5);
    double minX = -20.0, minZ = -20.0, maxX = 20.0, maxZ = 20.0;

    Value widthV = ev::getProperty(root.get(), "width");
    Value heightV = ev::getProperty(root.get(), "height");
    if (!ev::isUndefined(widthV) && !ev::isUndefined(heightV) && !ev::isObject(widthV) && !ev::isObject(heightV)) {
        double w = ev::toDouble(widthV);
        double h = ev::toDouble(heightV);
        double ox = getDoubleProperty(root.get(), "originX", 0.0);
        double oz = getDoubleProperty(root.get(), "originZ", 0.0);
        minX = ox;
        minZ = oz;
        maxX = ox + w * cellSize;
        maxZ = oz + h * cellSize;
    } else {
        minX = getDoubleProperty(root.get(), "minX", -20.0);
        minZ = getDoubleProperty(root.get(), "minZ", -20.0);
        maxX = getDoubleProperty(root.get(), "maxX", 20.0);
        maxZ = getDoubleProperty(root.get(), "maxZ", 20.0);
    }

    auto grid = std::make_unique<brogameagent::NavGrid>(
        static_cast<float>(minX), static_cast<float>(minZ),
        static_cast<float>(maxX), static_cast<float>(maxZ),
        static_cast<float>(cellSize));

    double padding = getDoubleProperty(root.get(), "padding", 0.0);

    Value obsArr = ev::getProperty(root.get(), "obstacles");
    if (ev::isObject(obsArr)) {
        auto boxes = parseAABBArray(obsArr);
        for (const auto& box : boxes) {
            grid->addObstacle(box, static_cast<float>(padding));
        }
    }

    Value fromPhys = ev::getProperty(root.get(), "fromPhysics");
    if (!ev::isUndefined(fromPhys) && !ev::isNull(fromPhys) && ev::toBool(fromPhys)) {
        auto* e = hostEngine();
        auto* world = e ? e->physicsWorld() : nullptr;
        if (world) {
            for (const auto& b : world->collectStaticBodies()) {
                if (b.isSensor) continue;
                if (b.min.GetX() <= minX && b.max.GetX() >= maxX &&
                    b.min.GetZ() <= minZ && b.max.GetZ() >= maxZ) continue;
                brogameagent::AABB box{
                    0.5f * (b.min.GetX() + b.max.GetX()),
                    0.5f * (b.min.GetZ() + b.max.GetZ()),
                    0.5f * (b.max.GetX() - b.min.GetX()),
                    0.5f * (b.max.GetZ() - b.min.GetZ()),
                };
                grid->addObstacle(box, static_cast<float>(padding));
            }
        }
    }

    return makeNavGridHandle(std::move(grid));
}

}  // namespace bro::bronze_host
