// Physics soft body bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

void decorateSoftBodyProto(ObjectBuilder& sbb) {
    sbb.accessor("body", [](Value self_, std::span<const Value>) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        return ev::fromDouble(sb->bodyTag);
    }, nullptr);

    sbb.accessor("vertexCount", [](Value self_, std::span<const Value>) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        int count = (w && sb->handle) ? w->softBodyVertexCount(sb->handle) : 0;
        return ev::fromDouble(count < 0 ? 0 : count);
    }, nullptr);

    sbb.def("vertices", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !sb->handle) return ev::null();
        std::vector<float> flat;
        if (!w->getSoftBodyVertices(sb->handle, flat)) return ev::null();
        return makeFloat32Array(flat.data(), flat.size());
    });

    sbb.def("topology", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !sb->handle) return ev::null();
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        if (!w->softBodyTopology(sb->handle, pos, idx)) return ev::null();
        ObjectBuilder res;
        res.set("positions", makeFloat32Array(pos.data(), pos.size()));
        res.set("indices", makeUint32Array(idx.data(), idx.size()));
        res.set("gridX", ev::fromDouble(sb->gridX));
        res.set("gridZ", ev::fromDouble(sb->gridZ));
        return res.get();
    });

    sbb.def("setVertex", 4, [](Value self_, std::span<const Value> args) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !sb->handle || args.size() < 4) return ev::fromBool(false);
        int idx = i32At(args, 0);
        double x = numAt(args, 1), y = numAt(args, 2), z = numAt(args, 3);
        return ev::fromBool(w->setSoftBodyVertexPosition(sb->handle, idx,
            JPH::RVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))));
    });

    sbb.def("setVertexVelocity", 4, [](Value self_, std::span<const Value> args) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !sb->handle || args.size() < 4) return ev::fromBool(false);
        int idx = i32At(args, 0);
        double x = numAt(args, 1), y = numAt(args, 2), z = numAt(args, 3);
        return ev::fromBool(w->setSoftBodyVertexVelocity(sb->handle, idx,
            JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))));
    });

    sbb.def("pin", 2, [](Value self_, std::span<const Value> args) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !sb->handle || args.empty()) return ev::fromBool(false);
        int idx = i32At(args, 0);
        bool pinned = args.size() < 2 ? true : boolAt(args, 1);
        return ev::fromBool(w->pinSoftBodyVertex(sb->handle, idx, pinned));
    });

    sbb.def("destroy", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsSoftBody* sb = unwrapSoftBody(self_);
        if (!sb) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (w && sb->handle) {
            w->destroySoftBody(sb->handle);
            g_phys.unregisterBody(sb->bodyTag);
        }
        sb->handle = 0;
        sb->bodyTag = -1;
        return ev::undefined();
    });
}

Value physicsCreateSoftBody(Value, std::span<const Value> a) {
    auto* world = getPhysicsWorld();
    if (!world) return ev::throwError("PhysicsWorld not available");
    if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createSoftBody(options) requires an object");

    ev::Persistent opts(a[0]);
    physics::SoftBodyOptions sopts;

    Value clothV = ev::getProperty(opts.get(), "cloth");
    Value meshV = ev::getProperty(opts.get(), "mesh");
    const bool hasCloth = ev::isObject(clothV);
    const bool hasMesh = ev::isObject(meshV);

    bool pinCorners = false;
    auto readPinned = [&](Value holder) {
        ev::Persistent hp(holder);
        Value pv = ev::getProperty(hp.get(), "pinned");
        if (!ev::isUndefined(pv) && !ev::isNull(pv)) {
            if (!ev::isObject(pv)) {
                std::string str = ev::toUtf8(pv);
                if (str == "corners") pinCorners = true;
            } else {
                readU32Vector(pv, sopts.pinned);
            }
        }
    };

    if (hasCloth && !hasMesh) {
        ev::Persistent cp(clothV);
        sopts.kind = physics::SoftBodyOptions::Cloth;
        sopts.gridX = static_cast<int>(getPropNumber(cp, "gridX", sopts.gridX));
        sopts.gridZ = static_cast<int>(getPropNumber(cp, "gridZ", sopts.gridZ));
        sopts.spacing = static_cast<float>(getPropNumber(cp, "spacing", sopts.spacing));
        sopts.mass = static_cast<float>(getPropNumber(cp, "mass", sopts.mass));
        if (sopts.gridX < 2 || sopts.gridZ < 2) return ev::throwTypeError("cloth.gridX/gridZ must be >= 2");
        readPinned(clothV);
        if (pinCorners) {
            uint32_t gx = static_cast<uint32_t>(sopts.gridX), gz = static_cast<uint32_t>(sopts.gridZ);
            sopts.pinned.insert(sopts.pinned.end(), { 0u, gx - 1, (gz - 1) * gx, gz * gx - 1 });
        }
    } else if (hasMesh && !hasCloth) {
        ev::Persistent mp(meshV);
        sopts.kind = physics::SoftBodyOptions::Mesh;
        std::vector<float> verts;
        readFloatVector(ev::getProperty(mp.get(), "vertices"), verts);
        readU32Vector(ev::getProperty(mp.get(), "indices"), sopts.indices);
        sopts.vertices.reserve(verts.size() / 3);
        for (size_t i = 0; i + 2 < verts.size(); i += 3)
            sopts.vertices.emplace_back(verts[i], verts[i+1], verts[i+2]);
        sopts.mass = static_cast<float>(getPropNumber(mp, "mass", sopts.mass));
        sopts.pressure = static_cast<float>(getPropNumber(mp, "pressure", sopts.pressure));
        readPinned(meshV);
        if (sopts.vertices.size() < 3 || sopts.indices.size() < 3)
            return ev::throwTypeError("mesh requires vertices (xyz triples) and indices (triangle list)");
    } else {
        return ev::throwTypeError("createSoftBody requires exactly one of cloth: {...} or mesh: {...}");
    }

    sopts.compliance = static_cast<float>(getPropNumber(opts, "compliance", sopts.compliance));
    sopts.shearCompliance = static_cast<float>(getPropNumber(opts, "shearCompliance", sopts.shearCompliance));
    sopts.bendCompliance = static_cast<float>(getPropNumber(opts, "bendCompliance", sopts.bendCompliance));
    sopts.numIterations = static_cast<int>(getPropNumber(opts, "numIterations", sopts.numIterations));
    sopts.friction = static_cast<float>(getPropNumber(opts, "friction", sopts.friction));
    sopts.restitution = static_cast<float>(getPropNumber(opts, "restitution", sopts.restitution));
    sopts.linearDamping = static_cast<float>(getPropNumber(opts, "linearDamping", sopts.linearDamping));
    sopts.gravityFactor = static_cast<float>(getPropNumber(opts, "gravityFactor", sopts.gravityFactor));
    sopts.vertexRadius = static_cast<float>(getPropNumber(opts, "vertexRadius", sopts.vertexRadius));
    sopts.updatePosition = getPropBool(opts, "updatePosition", sopts.updatePosition);
    sopts.doubleSided = getPropBool(opts, "doubleSided", sopts.doubleSided);
    sopts.allowSleeping = getPropBool(opts, "allowSleeping", sopts.allowSleeping);
    sopts.position = readRVec3(ev::getProperty(opts.get(), "position"));
    sopts.rotation = readQuat(ev::getProperty(opts.get(), "rotation"));

    Value layerVal = ev::getProperty(opts.get(), "layer");
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            int idx = 0;
            if (parseDecimalIndex(s, idx)) sopts.layer = idx;
            else sopts.layer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createSoftBody(sopts);
    if (!handle) return ev::throwError("Failed to create soft body");

    auto* sb = new HostPhysicsSoftBody();
    sb->handle = handle;
    sb->bodyTag = g_phys.registerBody(world->softBodyBody(handle));
    if (sopts.kind == physics::SoftBodyOptions::Cloth) {
        sb->gridX = std::max(2, sopts.gridX);
        sb->gridZ = std::max(2, sopts.gridZ);
    }

    ObjectBuilder sbb(g_softBodyClass.make(sb, [](void* p) {
        delete static_cast<HostPhysicsSoftBody*>(p);
    }));

    return sbb.get();
}

}  // namespace bro::bronze_host
