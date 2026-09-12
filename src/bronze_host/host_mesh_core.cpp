#if BRO_WITH_3D

#include "bronze_host/host_mesh_internal.h"
#include "util/asset_path.h"

namespace bro::bronze_host {

HostClass g_meshClass;

void hostMeshDtor(void* p) {
    delete static_cast<HostMesh*>(p);
}

Value wrapMesh(std::unique_ptr<bromesh::MeshData> data) {
    if (!data) return ev::undefined();
    auto* cell = new HostMesh();
    cell->mesh = std::move(data);
    return g_meshClass.make(cell, hostMeshDtor);
}

Value wrapMesh(bromesh::MeshData&& data) {
    auto* cell = new HostMesh();
    cell->mesh = std::make_unique<bromesh::MeshData>(std::move(data));
    return g_meshClass.make(cell, hostMeshDtor);
}

Value makeBBox(const bromath::AABB3& bb) {
    ObjectBuilder obj;
    std::vector<float> minArr = {bb.min.x, bb.min.y, bb.min.z};
    std::vector<float> maxArr = {bb.max.x, bb.max.y, bb.max.z};
    obj.set("min", hostArrayOf(3, [&minArr](size_t i) -> Value { return ev::fromDouble(minArr[i]); }));
    obj.set("max", hostArrayOf(3, [&maxArr](size_t i) -> Value { return ev::fromDouble(maxArr[i]); }));
    bromath::Vec3 c = bromath::acenter(bb);
    bromath::Vec3 e = bromath::aextent(bb);
    obj.set("centerX", ev::fromDouble(c.x));
    obj.set("centerY", ev::fromDouble(c.y));
    obj.set("centerZ", ev::fromDouble(c.z));
    obj.set("extentX", ev::fromDouble(e.x));
    obj.set("extentY", ev::fromDouble(e.y));
    obj.set("extentZ", ev::fromDouble(e.z));
    obj.set("radius", ev::fromDouble(bromath::vlen(e) * 0.5f));
    return obj.get();
}

Value makeRayHit(const bromesh::RayHit& h) {
    if (!h.hit) return ev::null();
    ObjectBuilder obj;
    obj.set("distance", ev::fromDouble(h.distance));
    obj.set("position", hostArrayOf(3, [&h](size_t i) -> Value { return ev::fromDouble(h.position[i]); }));
    obj.set("normal", hostArrayOf(3, [&h](size_t i) -> Value { return ev::fromDouble(h.normal[i]); }));
    obj.set("triangleIndex", ev::fromDouble(static_cast<double>(h.triangleIndex)));
    obj.set("baryU", ev::fromDouble(h.baryU));
    obj.set("baryV", ev::fromDouble(h.baryV));
    obj.set("baryW", ev::fromDouble(h.baryW));
    return obj.get();
}

Value makeTextureBuffer(const bromesh::TextureBuffer& tb) {
    ObjectBuilder obj;
    obj.set("width", ev::fromDouble(tb.width));
    obj.set("height", ev::fromDouble(tb.height));
    obj.set("channels", ev::fromDouble(tb.channels));
    obj.set("pixels", makeFloat32Array(tb.pixels.data(), tb.pixels.size()));
    return obj.get();
}

std::string resolveMeshWritePath(const std::string& path) {
    return bro::util::resolveAssetWritePath(path);
}

void decorateMeshCore(ObjectBuilder& b) {
    // positions
    b.accessor("positions",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (!m) return ev::undefined();
            return makeFloat32Array(m->positions.data(), m->positions.size());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (m && !a.empty()) readFloatVector(a[0], m->positions);
            return ev::undefined();
        });

    // normals
    b.accessor("normals",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (!m) return ev::undefined();
            return makeFloat32Array(m->normals.data(), m->normals.size());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (m && !a.empty()) readFloatVector(a[0], m->normals);
            return ev::undefined();
        });

    // uvs
    b.accessor("uvs",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (!m) return ev::undefined();
            return makeFloat32Array(m->uvs.data(), m->uvs.size());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (m && !a.empty()) readFloatVector(a[0], m->uvs);
            return ev::undefined();
        });

    // colors
    b.accessor("colors",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (!m) return ev::undefined();
            return makeFloat32Array(m->colors.data(), m->colors.size());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (m && !a.empty()) readFloatVector(a[0], m->colors);
            return ev::undefined();
        });

    // indices
    b.accessor("indices",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (!m) return ev::undefined();
            return makeUint32Array(m->indices.data(), m->indices.size());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            auto* m = hostMeshDataOf(self_);
            if (m && !a.empty()) readU32Vector(a[0], m->indices);
            return ev::undefined();
        });

    // vertexCount
    b.accessor("vertexCount",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromDouble(m ? static_cast<double>(m->vertexCount()) : 0.0);
        }, nullptr);

    // triangleCount
    b.accessor("triangleCount",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromDouble(m ? static_cast<double>(m->triangleCount()) : 0.0);
        }, nullptr);

    // hasNormals
    b.accessor("hasNormals",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromBool(m && m->hasNormals());
        }, nullptr);

    // hasUVs
    b.accessor("hasUVs",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromBool(m && m->hasUVs());
        }, nullptr);

    // hasColors
    b.accessor("hasColors",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromBool(m && m->hasColors());
        }, nullptr);

    // empty
    b.accessor("empty",
        [](Value self_, std::span<const Value>) -> Value {
            auto* m = hostMeshDataOf(self_);
            return ev::fromBool(!m || m->empty());
        }, nullptr);

    // clone
    b.def("clone", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        return wrapMesh(bromesh::MeshData(*m));
    });
}

void installMeshGlobals() {
    g_meshClass.install("Mesh", 1,
        [](Value, std::span<const Value> a) -> Value {
            auto md = std::make_unique<bromesh::MeshData>();
            if (!a.empty() && ev::isObject(a[0])) {
                readFloatVector(ev::getProperty(a[0], "positions"), md->positions);
                readFloatVector(ev::getProperty(a[0], "normals"), md->normals);
                readFloatVector(ev::getProperty(a[0], "uvs"), md->uvs);
                readFloatVector(ev::getProperty(a[0], "colors"), md->colors);
                readU32Vector(ev::getProperty(a[0], "indices"), md->indices);
            }
            return wrapMesh(std::move(md));
        },
        [](ObjectBuilder& b) {
            decorateMeshCore(b);
            decorateMeshManipulate(b, g_meshClass);
            decorateMeshAnalysis(b, g_meshClass);
            decorateMeshOptimize(b, g_meshClass);
        });

    decorateMeshPrimitives(g_meshClass);

    installMeshBVH();
    installCapsuleField();
    installProgressiveMesh();
    installPolyMesh();
    installLSystem();
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
