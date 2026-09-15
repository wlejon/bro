// `__bro_native.mesh` — the C entry points behind bro.mesh, Mesh and MeshBVH
// (docs/mesh-api.js), on bronze's native mechanism. host_natives.h states
// the convention; what this file adds to it is the two NATIVE CLASSES:
//
//   * `__bro_native.mesh.Mesh` is a registered constructor whose handle owns
//     a bromesh::MeshData (destructor: delete). Its members are NOT native
//     methods but namespace FUNCTIONS taking the handle as their first,
//     class-typed parameter — `__bro_native.mesh.vertexCount(m)` — because
//     the compiler lowers a method to a direct call only on a receiver whose
//     class it can see (a binding it saw constructed), and the receiver
//     inside js/mesh.js's accessors is `this`, which it cannot. A function
//     with a `__bro_native.mesh.Mesh` parameter is a direct call from
//     anywhere, and the runtime's one-word tag compare at the call site is
//     what turns a wrong-class handle or a plain object into a TypeError
//     naming the class.
//
//   * `__bro_native.mesh.MeshBVH` is the second class: a bromesh::MeshBVH
//     built over its OWN COPY of the mesh, so `raycast(origin, direction,
//     maxDist)` needs no second argument and cannot outlive the mesh it
//     indexes.
//
// TYPED-ARRAY RETURNS (`f32[]`, `u32[]`, `u8[]`) are where the ownership
// rule is decided per native, in the trailing bronze_native_buffer:
//
//   COPY (release left null)  every attribute getter — positions, normals,
//     uvs, colors, indices — and the six-float bounds. The storage is the
//     mesh's own std::vector, which the next in-place operation reallocates;
//     the program gets a JS-owned copy, and mutating it never reaches the
//     mesh (the setters are the way back in). bvhBounds, dracoAttributeBytes
//     likewise: storage owned by something else.
//   TRANSFER (release set)  every FRESH result — triangleAreas,
//     computeUVDistortion, the four meshlet blocks, encodeDraco. bromesh
//     hands back a std::vector nobody else holds; it moves to the heap, the
//     program's typed array views its bytes in place, and the release
//     deletes it when the ArrayBuffer is collected.
//
// COMPOUND RESULTS cross as the convention says: a ray hit as JSON `str`
// (js/mesh.js parses it into MeshBVHIntersectResult), a list of meshes
// (splitComponents, convexDecomposition, the Draco attributes) as a count
// over a per-thread snapshot read back by index, a list ARGUMENT (merge) as
// begin/add/end calls.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"

#if BRO_WITH_3D

#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/convex_decomposition.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/sample.h>
#include <bromesh/csg/boolean.h>
#include <bromesh/isosurface/dual_contouring.h>
#include <bromesh/isosurface/marching_cubes.h>
#include <bromesh/isosurface/surface_nets.h>
#include <bromesh/manipulation/merge.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/remesh.h>
#include <bromesh/manipulation/repair.h>
#include <bromesh/manipulation/simplify.h>
#include <bromesh/manipulation/smooth.h>
#include <bromesh/manipulation/split_components.h>
#include <bromesh/manipulation/subdivide.h>
#include <bromesh/manipulation/sweep.h>
#include <bromesh/manipulation/transform.h>
#include <bromesh/manipulation/weld.h>
#include <bromesh/mesh_data.h>
#include <bromesh/optimization/meshlets.h>
#include <bromesh/optimization/optimize.h>
#include <bromesh/primitives/par_primitives.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/uv/projection.h>
#include <bromesh/uv/unwrap.h>
#include <bromesh/uv/uv_metrics.h>
#if BROMESH_HAS_DRACO
#include <bromesh/io/draco.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

using bromesh::MeshData;

constexpr const char* kMesh = "__bro_native.mesh.Mesh";
constexpr const char* kBVH = "__bro_native.mesh.MeshBVH";

MeshData& M(void* handle) { return *static_cast<MeshData*>(handle); }
void* take(MeshData&& m) { return new MeshData(std::move(m)); }
void meshFree(void* p) { delete static_cast<MeshData*>(p); }

// ---- the two buffer-return modes -------------------------------------------

template <typename T>
void copyOut(const std::vector<T>& v, bronze_native_buffer* out) {
    // Storage the mesh keeps: the runtime copies before the next operation
    // could reallocate it.
    out->data = v.empty() ? nullptr : const_cast<T*>(v.data());
    out->length = static_cast<uint32_t>(v.size());
}

template <typename T>
void releaseVector(void* ctx) { delete static_cast<std::vector<T>*>(ctx); }

template <typename T>
void transferOut(std::vector<T>&& v, bronze_native_buffer* out) {
    // A fresh result nobody else holds: the program's array views it in
    // place and the release deletes it when the buffer is collected.
    if (v.empty()) return;
    auto* owned = new std::vector<T>(std::move(v));
    out->data = owned->data();
    out->length = static_cast<uint32_t>(owned->size());
    out->release = &releaseVector<T>;
    out->ctx = owned;
}

// ---- JSON pieces -----------------------------------------------------------

std::string num(double d) {
    if (!std::isfinite(d)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", d);
    return buf;
}

std::string triple(const float* v) {
    return "[" + num(v[0]) + "," + num(v[1]) + "," + num(v[2]) + "]";
}

// A RayHit as the MeshBVHIntersectResult dictionary, "" for a miss. `uv` is
// the mesh's own UV interpolated at the hit when it has UVs.
std::string hitJson(const MeshData& m, const bromesh::RayHit& h) {
    if (!h.hit) return std::string();
    std::string s = "{\"hit\":true,\"distance\":" + num(h.distance) +
                    ",\"triangle\":" + std::to_string(h.triangleIndex) +
                    ",\"point\":" + triple(h.position) + ",\"normal\":" + triple(h.normal) +
                    ",\"barycentric\":[" + num(h.baryU) + "," + num(h.baryV) + "," + num(h.baryW) + "]";
    const size_t t = static_cast<size_t>(h.triangleIndex);
    if (m.hasUVs() && t * 3 + 2 < m.indices.size()) {
        const uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        const float u = h.baryU * m.uvs[i0 * 2] + h.baryV * m.uvs[i1 * 2] + h.baryW * m.uvs[i2 * 2];
        const float v = h.baryU * m.uvs[i0 * 2 + 1] + h.baryV * m.uvs[i1 * 2 + 1] + h.baryW * m.uvs[i2 * 2 + 1];
        s += ",\"uv\":[" + num(u) + "," + num(v) + "]";
    }
    s += "}";
    return s;
}

// ---- the Mesh class --------------------------------------------------------

// Every array is validated against the vertex count the positions define,
// so a handle never holds a MeshData that fails validate(): the analysis
// and manipulation code indexes without checking.
void* meshNew(const float* p, uint32_t pn, const float* n, uint32_t nn, const float* u, uint32_t un,
              const float* c, uint32_t cn, const uint32_t* idx, uint32_t in) {
    if (pn % 3 != 0) {
        ev::throwTypeError("Mesh: positions length must be a multiple of 3");
        return nullptr;
    }
    const uint32_t verts = pn / 3;
    if (nn != 0 && nn != pn) {
        ev::throwTypeError("Mesh: normals must hold one xyz per vertex");
        return nullptr;
    }
    if (un != 0 && un != verts * 2) {
        ev::throwTypeError("Mesh: uvs must hold one uv per vertex");
        return nullptr;
    }
    if (cn != 0 && cn != verts * 4) {
        ev::throwTypeError("Mesh: colors must hold one rgba per vertex");
        return nullptr;
    }
    if (in % 3 != 0) {
        ev::throwTypeError("Mesh: indices length must be a multiple of 3");
        return nullptr;
    }
    for (uint32_t i = 0; i < in; ++i) {
        if (idx[i] >= verts) {
            ev::throwRangeError("Mesh: index " + std::to_string(idx[i]) + " is out of range for " +
                                std::to_string(verts) + " vertices");
            return nullptr;
        }
    }
    auto* m = new MeshData();
    m->positions.assign(p, p + pn);
    m->normals.assign(n, n + nn);
    m->uvs.assign(u, u + un);
    m->colors.assign(c, c + cn);
    m->indices.assign(idx, idx + in);
    return m;
}

double vertexCount(void* m) { return static_cast<double>(M(m).vertexCount()); }
double triangleCount(void* m) { return static_cast<double>(M(m).triangleCount()); }
bool hasNormals(void* m) { return M(m).hasNormals(); }
bool hasUVs(void* m) { return M(m).hasUVs(); }
bool hasColors(void* m) { return M(m).hasColors(); }
bool isEmpty(void* m) { return M(m).empty(); }

// Attribute reads: COPY mode (the vectors are the mesh's own).
void positions(void* m, bronze_native_buffer* out) { copyOut(M(m).positions, out); }
void normals(void* m, bronze_native_buffer* out) { copyOut(M(m).normals, out); }
void uvs(void* m, bronze_native_buffer* out) { copyOut(M(m).uvs, out); }
void colors(void* m, bronze_native_buffer* out) { copyOut(M(m).colors, out); }
void indices(void* m, bronze_native_buffer* out) { copyOut(M(m).indices, out); }

// Attribute writes, validated the way the constructor validates.
void setPositions(void* m, const float* d, uint32_t n) {
    if (n % 3 != 0) { ev::throwTypeError("Mesh.positions: length must be a multiple of 3"); return; }
    M(m).positions.assign(d, d + n);
}
void setNormals(void* m, const float* d, uint32_t n) {
    if (n != 0 && n != M(m).positions.size()) {
        ev::throwTypeError("Mesh.normals: must hold one xyz per vertex");
        return;
    }
    M(m).normals.assign(d, d + n);
}
void setUVs(void* m, const float* d, uint32_t n) {
    if (n != 0 && n != M(m).vertexCount() * 2) {
        ev::throwTypeError("Mesh.uvs: must hold one uv per vertex");
        return;
    }
    M(m).uvs.assign(d, d + n);
}
void setColors(void* m, const float* d, uint32_t n) {
    if (n != 0 && n != M(m).vertexCount() * 4) {
        ev::throwTypeError("Mesh.colors: must hold one rgba per vertex");
        return;
    }
    M(m).colors.assign(d, d + n);
}
void setIndices(void* m, const uint32_t* d, uint32_t n) {
    if (n % 3 != 0) { ev::throwTypeError("Mesh.indices: length must be a multiple of 3"); return; }
    const size_t verts = M(m).vertexCount();
    for (uint32_t i = 0; i < n; ++i) {
        if (d[i] >= verts) {
            ev::throwRangeError("Mesh.indices: index " + std::to_string(d[i]) + " is out of range");
            return;
        }
    }
    M(m).indices.assign(d, d + n);
}

void* clone(void* m) { return new MeshData(M(m)); }

// ---- in-place manipulation -------------------------------------------------

void translate(void* m, double dx, double dy, double dz) {
    bromesh::translateMesh(M(m), static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
}
void scale(void* m, double sx, double sy, double sz) {
    bromesh::scaleMesh(M(m), static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz));
}
void rotate(void* m, double ax, double ay, double az, double angle) {
    bromesh::rotateMesh(M(m), static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(az),
                        static_cast<float>(angle));
}
void center(void* m) { bromesh::centerMesh(M(m)); }
void fitToBox(void* m, double size) {
    MeshData& mesh = M(m);
    if (mesh.empty()) return;
    bromesh::centerMesh(mesh);
    const bromath::AABB3 bb = bromesh::computeBBox(mesh);
    const float ex = bb.max.x - bb.min.x, ey = bb.max.y - bb.min.y, ez = bb.max.z - bb.min.z;
    const float longest = std::fmax(ex, std::fmax(ey, ez));
    if (longest > 0.0f) bromesh::scaleMesh(mesh, static_cast<float>(size) / longest);
}
void transform(void* m, const float* mat, uint32_t n) {
    if (n != 16) {
        ev::throwTypeError("Mesh.transform: matrix must have 16 elements, got " + std::to_string(n));
        return;
    }
    bromesh::transformMesh(M(m), mat);
}
void computeNormals(void* m, double creaseAngleDeg) {
    if (creaseAngleDeg > 0.0) {
        M(m) = bromesh::computeCreaseNormals(M(m), static_cast<float>(creaseAngleDeg));
    } else {
        bromesh::computeNormals(M(m));
    }
}
void invertNormals(void* m) {
    for (float& f : M(m).normals) f = -f;
}
void flipFaces(void* m) {
    auto& idx = M(m).indices;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
}
void weld(void* m, double eps) { M(m) = bromesh::weldVertices(M(m), static_cast<float>(eps)); }
void simplify(void* m, double ratio, double targetError) {
    M(m) = bromesh::simplify(M(m), static_cast<float>(ratio), static_cast<float>(targetError));
}
void subdivideLoop(void* m, int32_t iterations) { M(m) = bromesh::subdivideLoop(M(m), iterations); }
void subdivideCatmullClark(void* m, int32_t iterations) {
    M(m) = bromesh::subdivideCatmullClark(M(m), iterations);
}
void smooth(void* m, double lambda, int32_t iterations) {
    bromesh::smoothLaplacian(M(m), static_cast<float>(lambda), iterations);
}
void remesh(void* m, double targetEdgeLength) {
    M(m) = bromesh::remeshIsotropic(M(m), static_cast<float>(targetEdgeLength));
}
void repair(void* m) {
    MeshData& mesh = M(m);
    mesh = bromesh::removeDegenerateTriangles(mesh);
    mesh = bromesh::removeDuplicateTriangles(mesh);
    mesh = bromesh::fillHoles(mesh);
}
void optimize(void* m) {
    bromesh::optimizeVertexCache(M(m));
    bromesh::optimizeOverdraw(M(m));
    bromesh::optimizeVertexFetch(M(m));
}

// A list of meshes out: count over a per-thread snapshot, then takePiece(i)
// moves each into its own handle.
thread_local std::vector<MeshData> g_pieces;

int32_t splitComponents(void* m) {
    g_pieces = bromesh::splitConnectedComponents(M(m));
    return static_cast<int32_t>(g_pieces.size());
}
int32_t convexDecomposition(void* m, int32_t maxHulls, int32_t maxVerticesPerHull, double resolution,
                            double minVolumePerHull) {
    bromesh::ConvexDecompParams p;
    if (maxHulls > 0) p.maxHulls = maxHulls;
    if (maxVerticesPerHull > 0) p.maxVerticesPerHull = maxVerticesPerHull;
    if (resolution > 0.0) p.resolution = static_cast<float>(resolution);
    if (minVolumePerHull > 0.0) p.minVolumePerHull = static_cast<float>(minVolumePerHull);
    g_pieces = bromesh::convexDecomposition(M(m), p);
    return static_cast<int32_t>(g_pieces.size());
}
void* takePiece(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= g_pieces.size()) {
        ev::throwRangeError("mesh piece " + std::to_string(i) + " is not in the snapshot");
        return nullptr;
    }
    return take(std::move(g_pieces[static_cast<size_t>(i)]));
}

void* convexHull(void* m) { return take(bromesh::convexHull(M(m))); }
void* booleanUnion(void* a, void* b) { return take(bromesh::booleanUnion(M(a), M(b))); }
void* booleanDifference(void* a, void* b) { return take(bromesh::booleanDifference(M(a), M(b))); }
void* booleanIntersection(void* a, void* b) { return take(bromesh::booleanIntersection(M(a), M(b))); }

// ---- UVs -------------------------------------------------------------------

bool unwrapUVs(void* m) {
    bromesh::UnwrapParams up;
    bromesh::PackParams pp;
    return bromesh::unwrapUVs(M(m), up, pp).success;
}
// `type` is the ProjectionType ordinal js/mesh.js maps the name to.
void projectUVs(void* m, int32_t type, double scale) {
    if (type < 0 || type > static_cast<int32_t>(bromesh::ProjectionType::Spherical)) {
        ev::throwRangeError("Mesh.projectUVs: unknown projection " + std::to_string(type));
        return;
    }
    bromesh::projectUVs(M(m), static_cast<bromesh::ProjectionType>(type),
                        static_cast<float>(scale > 0.0 ? scale : 1.0));
}
// TRANSFER: three floats per triangle (stretch, areaDistortion, angleDistortion).
void computeUVDistortion(void* m, bronze_native_buffer* out) {
    const auto d = bromesh::computeUVDistortion(M(m));
    std::vector<float> flat;
    flat.reserve(d.size() * 3);
    for (const auto& e : d) {
        flat.push_back(e.stretch);
        flat.push_back(e.areaDistortion);
        flat.push_back(e.angleDistortion);
    }
    transferOut(std::move(flat), out);
}
const char* measureUVQuality(void* m) {
    const bromesh::UVMetrics q = bromesh::measureUVQuality(M(m));
    return natives::strResult(
        "{\"avgStretch\":" + num(q.avgStretch) + ",\"maxStretch\":" + num(q.maxStretch) +
        ",\"avgAreaDistortion\":" + num(q.avgAreaDistortion) +
        ",\"maxAreaDistortion\":" + num(q.maxAreaDistortion) +
        ",\"avgAngleDistortion\":" + num(q.avgAngleDistortion) +
        ",\"maxAngleDistortion\":" + num(q.maxAngleDistortion) +
        ",\"uvSpaceUsage\":" + num(q.uvSpaceUsage) +
        ",\"triangleCount\":" + std::to_string(q.triangleCount) + "}");
}

// ---- meshlets --------------------------------------------------------------
//
// One build, four TRANSFER blocks read back once each: the concatenated
// vertex remap (u32), the concatenated micro-indices (u8), and per meshlet a
// 64-byte record — u32 vertexOffset, vertexCount, triangleOffset (into the
// micro-index bytes), triangleCount; f32 center[3], radius, coneApex[3],
// coneAxis[3], coneCutoff; one u32 pad — as raw bytes js/mesh.js exposes as
// the `meshlets` ArrayBuffer.

thread_local std::vector<uint32_t> g_meshletVertices;
thread_local std::vector<uint8_t> g_meshletTriangles;
thread_local std::vector<uint8_t> g_meshletRecords;

int32_t buildMeshlets(void* m, int32_t maxVertices, int32_t maxTriangles, double coneWeight) {
    bromesh::MeshletParams p;
    if (maxVertices > 0) p.maxVertices = static_cast<size_t>(maxVertices);
    if (maxTriangles > 0) p.maxTriangles = static_cast<size_t>(maxTriangles);
    if (coneWeight >= 0.0) p.coneWeight = static_cast<float>(coneWeight);
    const auto ml = bromesh::buildMeshlets(M(m), p);
    g_meshletVertices.clear();
    g_meshletTriangles.clear();
    g_meshletRecords.clear();
    g_meshletRecords.reserve(ml.size() * 64);
    auto putU32 = [](uint32_t v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        g_meshletRecords.insert(g_meshletRecords.end(), b, b + 4);
    };
    auto putF32 = [](float v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        g_meshletRecords.insert(g_meshletRecords.end(), b, b + 4);
    };
    for (const auto& item : ml) {
        putU32(static_cast<uint32_t>(g_meshletVertices.size()));
        putU32(static_cast<uint32_t>(item.vertices.size()));
        putU32(static_cast<uint32_t>(g_meshletTriangles.size()));
        putU32(static_cast<uint32_t>(item.triangles.size() / 3));
        for (float f : item.bounds.center) putF32(f);
        putF32(item.bounds.radius);
        for (float f : item.bounds.coneApex) putF32(f);
        for (float f : item.bounds.coneAxis) putF32(f);
        putF32(item.bounds.coneCutoff);
        putU32(0);
        g_meshletVertices.insert(g_meshletVertices.end(), item.vertices.begin(), item.vertices.end());
        g_meshletTriangles.insert(g_meshletTriangles.end(), item.triangles.begin(), item.triangles.end());
    }
    return static_cast<int32_t>(ml.size());
}
void meshletVertices(bronze_native_buffer* out) { transferOut(std::move(g_meshletVertices), out); }
void meshletTriangles(bronze_native_buffer* out) { transferOut(std::move(g_meshletTriangles), out); }
void meshletRecords(bronze_native_buffer* out) { transferOut(std::move(g_meshletRecords), out); }

// ---- analysis --------------------------------------------------------------

thread_local float g_bounds[6];
void bounds(void* m, bronze_native_buffer* out) {
    const bromath::AABB3 bb = bromesh::computeBBox(M(m));
    g_bounds[0] = bb.min.x; g_bounds[1] = bb.min.y; g_bounds[2] = bb.min.z;
    g_bounds[3] = bb.max.x; g_bounds[4] = bb.max.y; g_bounds[5] = bb.max.z;
    out->data = g_bounds;  // COPY: a per-thread scratch
    out->length = 6;
}
double surfaceArea(void* m) { return bromesh::computeSurfaceArea(M(m)); }
double volume(void* m) { return bromesh::computeVolume(M(m)); }
bool isManifold(void* m) { return bromesh::isManifold(M(m)); }
// TRANSFER: one float per triangle.
void triangleAreas(void* m, bronze_native_buffer* out) {
    transferOut(bromesh::computeTriangleAreas(M(m)), out);
}
const char* raycast(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                    double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    return natives::strResult(hitJson(M(m), bromesh::raycast(M(m), o, d, static_cast<float>(maxDist))));
}

// ---- primitives ------------------------------------------------------------

void* box(double hw, double hh, double hd) {
    return take(bromesh::box(static_cast<float>(hw), static_cast<float>(hh), static_cast<float>(hd)));
}
void* sphere(double r, int32_t segments, int32_t rings) {
    return take(bromesh::sphere(static_cast<float>(r), segments, rings));
}
void* cylinder(double r, double hh, int32_t segments) {
    return take(bromesh::cylinder(static_cast<float>(r), static_cast<float>(hh), segments));
}
void* capsule(double r, double hh, int32_t segments, int32_t rings) {
    return take(bromesh::capsule(static_cast<float>(r), static_cast<float>(hh), segments, rings));
}
void* cone(double r, double h, int32_t segments, int32_t stacks) {
    return take(bromesh::cone(static_cast<float>(r), static_cast<float>(h), segments, stacks, true));
}
void* plane(double hw, double hd, int32_t subX, int32_t subZ) {
    return take(bromesh::plane(static_cast<float>(hw), static_cast<float>(hd), subX, subZ));
}
void* torus(double major, double minor, int32_t majorSeg, int32_t minorSeg) {
    return take(bromesh::torus(static_cast<float>(major), static_cast<float>(minor), majorSeg, minorSeg));
}
// The Platonic solids come out of par_shapes at unit size; `radius` scales.
void* scaled(MeshData m, double radius) {
    if (radius > 0.0 && radius != 1.0) bromesh::scaleMesh(m, static_cast<float>(radius));
    return take(std::move(m));
}
void* icosahedron(double radius) { return scaled(bromesh::icosahedron(), radius); }
void* dodecahedron(double radius) { return scaled(bromesh::dodecahedron(), radius); }
void* octahedron(double radius) { return scaled(bromesh::octahedron(), radius); }
void* tetrahedron(double radius) { return scaled(bromesh::tetrahedron(), radius); }
void* disk(double r, int32_t segments) { return take(bromesh::disc(static_cast<float>(r), segments)); }
void* geodesicSphere(double r, int32_t nsub) {
    return take(bromesh::geodesicSphere(static_cast<float>(r), nsub));
}
void* rock(double r, int32_t seed, int32_t nsub) {
    return take(bromesh::rock(static_cast<float>(r), seed, nsub));
}
void* blob(double r, int32_t seed, int32_t nsub, double sx, double sy, double sz, double cx, double cy,
           double cz) {
    return take(bromesh::blob(static_cast<float>(r), seed, nsub, static_cast<float>(sx),
                              static_cast<float>(sy), static_cast<float>(sz), static_cast<float>(cx),
                              static_cast<float>(cy), static_cast<float>(cz)));
}
// `points` is the path as flat xyz triples.
void* tube(const float* points, uint32_t n, double radius, int32_t sides) {
    if (n % 3 != 0 || n < 6) {
        ev::throwTypeError("Mesh.tube: points must hold at least two xyz triples");
        return nullptr;
    }
    std::vector<bromath::Vec3> path;
    path.reserve(n / 3);
    for (uint32_t i = 0; i < n; i += 3) path.push_back({points[i], points[i + 1], points[i + 2]});
    bromesh::TubeOptions opts;
    if (sides > 2) opts.sides = sides;
    return take(bromesh::tube(path, std::vector<float>{static_cast<float>(radius)}, opts));
}

// merge: a list argument as begin / add / end.
thread_local std::vector<MeshData> g_mergeList;
void mergeBegin() { g_mergeList.clear(); }
void mergeAdd(void* m) { g_mergeList.push_back(M(m)); }
void* mergeEnd() {
    MeshData merged = bromesh::mergeMeshes(g_mergeList);
    g_mergeList.clear();
    return take(std::move(merged));
}

// ---- isosurfaces -----------------------------------------------------------

bool fieldFits(const char* what, uint32_t n, int32_t gx, int32_t gy, int32_t gz) {
    if (gx <= 0 || gy <= 0 || gz <= 0 ||
        static_cast<uint64_t>(n) != static_cast<uint64_t>(gx) * gy * gz) {
        ev::throwTypeError(std::string("Mesh.") + what + ": field length must be dimX*dimY*dimZ");
        return false;
    }
    return true;
}
void* marchingCubes(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso) {
    if (!fieldFits("marchingCubes", n, gx, gy, gz)) return nullptr;
    return take(bromesh::marchingCubes(field, gx, gy, gz, static_cast<float>(iso)));
}
void* surfaceNets(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso) {
    if (!fieldFits("surfaceNets", n, gx, gy, gz)) return nullptr;
    return take(bromesh::surfaceNets(field, gx, gy, gz, static_cast<float>(iso)));
}
void* dualContouring(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso) {
    if (!fieldFits("dualContouring", n, gx, gy, gz)) return nullptr;
    return take(bromesh::dualContour(field, gx, gy, gz, static_cast<float>(iso)));
}

// ---- Draco -----------------------------------------------------------------

#if BROMESH_HAS_DRACO
thread_local bromesh::DracoDecoded g_draco;

// "" on success; the decoder's message otherwise. The mesh and attributes
// wait in the snapshot for dracoTakeMesh / dracoAttribute*.
const char* dracoDecode(const uint8_t* bytes, uint32_t n) {
    g_draco = bromesh::decodeDraco(bytes, n);
    return natives::strResult(g_draco.error);
}
void* dracoTakeMesh() { return take(std::move(g_draco.mesh)); }
int32_t dracoAttributeCount() { return static_cast<int32_t>(g_draco.attributes.size()); }
const char* dracoAttributeInfo(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= g_draco.attributes.size()) {
        ev::throwRangeError("Draco attribute " + std::to_string(i) + " is not in the snapshot");
        return "";
    }
    const auto& a = g_draco.attributes[static_cast<size_t>(i)];
    return natives::strResult("{\"type\":\"" + a.type + "\",\"uniqueId\":" + std::to_string(a.uniqueId) +
                              ",\"components\":" + std::to_string(a.components) +
                              ",\"count\":" + std::to_string(a.count) +
                              ",\"kind\":" + std::to_string(static_cast<int>(a.kind)) + "}");
}
// COPY: the bytes belong to the snapshot.
void dracoAttributeBytes(int32_t i, bronze_native_buffer* out) {
    if (i < 0 || static_cast<size_t>(i) >= g_draco.attributes.size()) {
        ev::throwRangeError("Draco attribute " + std::to_string(i) + " is not in the snapshot");
        return;
    }
    copyOut(g_draco.attributes[static_cast<size_t>(i)].bytes, out);
}
// TRANSFER: the encoder's fresh buffer.
void dracoEncode(void* m, int32_t positionBits, int32_t normalBits, int32_t uvBits, int32_t colorBits,
                 int32_t speed, bronze_native_buffer* out) {
    bromesh::DracoEncodeOptions o;
    if (positionBits > 0) o.positionBits = positionBits;
    if (normalBits > 0) o.normalBits = normalBits;
    if (uvBits > 0) o.uvBits = uvBits;
    if (colorBits > 0) o.colorBits = colorBits;
    if (speed >= 0) o.speed = speed;
    std::string err;
    std::vector<uint8_t> bytes = bromesh::encodeDraco(M(m), o, &err);
    if (bytes.empty() && !err.empty()) {
        ev::throwError("Mesh.encodeDraco: " + err);
        return;
    }
    transferOut(std::move(bytes), out);
}
#endif

// ---- the MeshBVH class -----------------------------------------------------

struct BvhCell {
    MeshData mesh;  // its own copy: the index can never outlive what it indexes
    bromesh::MeshBVH bvh;
};

void* bvhNew(void* mesh, int32_t leafSize) {
    auto* cell = new BvhCell{M(mesh), bromesh::MeshBVH()};
    cell->bvh = bromesh::MeshBVH::build(cell->mesh, leafSize > 0 ? leafSize : 8);
    return cell;
}
void bvhFree(void* p) { delete static_cast<BvhCell*>(p); }
BvhCell& B(void* p) { return *static_cast<BvhCell*>(p); }

const char* bvhRaycast(void* b, double ox, double oy, double oz, double dx, double dy, double dz,
                       double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    BvhCell& cell = B(b);
    return natives::strResult(
        hitJson(cell.mesh, cell.bvh.raycast(cell.mesh, o, d, static_cast<float>(maxDist))));
}
bool bvhRaycastTest(void* b, double ox, double oy, double oz, double dx, double dy, double dz,
                    double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    BvhCell& cell = B(b);
    return cell.bvh.raycastTest(cell.mesh, o, d, static_cast<float>(maxDist));
}
bool bvhEmpty(void* b) { return B(b).bvh.empty(); }
double bvhNodeCount(void* b) { return static_cast<double>(B(b).bvh.nodeCount()); }
double bvhTriangleCount(void* b) { return static_cast<double>(B(b).bvh.triangleCount()); }
void bvhBounds(void* b, bronze_native_buffer* out) {
    const bromath::AABB3 bb = B(b).bvh.bounds();
    g_bounds[0] = bb.min.x; g_bounds[1] = bb.min.y; g_bounds[2] = bb.min.z;
    g_bounds[3] = bb.max.x; g_bounds[4] = bb.max.y; g_bounds[5] = bb.max.z;
    out->data = g_bounds;  // COPY
    out->length = 6;
}

}  // namespace

bool registerMeshNatives(std::string* error) {
    using namespace natives;
    auto F = [](auto* f) { return reinterpret_cast<void*>(f); };
    // The full paths, kept for the registration's duration (registerNative
    // copies them); a deque so an earlier c_str() never moves.
    const char* ns = "__bro_native.mesh.";
    auto P = [&](const char* name) {
        thread_local std::deque<std::string> keep;
        keep.push_back(std::string(ns) + name);
        return keep.back().c_str();
    };
    bool ok =
        ctor(kMesh, F(&meshNew), &meshFree, {"f32[]", "f32[]", "f32[]", "f32[]", "u32[]"}, error) &&
        fn(P("vertexCount"), F(&vertexCount), "f64", {kMesh}, error) &&
        fn(P("triangleCount"), F(&triangleCount), "f64", {kMesh}, error) &&
        fn(P("hasNormals"), F(&hasNormals), "bool", {kMesh}, error) &&
        fn(P("hasUVs"), F(&hasUVs), "bool", {kMesh}, error) &&
        fn(P("hasColors"), F(&hasColors), "bool", {kMesh}, error) &&
        fn(P("empty"), F(&isEmpty), "bool", {kMesh}, error) &&
        fn(P("positions"), F(&positions), "f32[]", {kMesh}, error) &&
        fn(P("normals"), F(&normals), "f32[]", {kMesh}, error) &&
        fn(P("uvs"), F(&uvs), "f32[]", {kMesh}, error) &&
        fn(P("colors"), F(&colors), "f32[]", {kMesh}, error) &&
        fn(P("indices"), F(&indices), "u32[]", {kMesh}, error) &&
        fn(P("setPositions"), F(&setPositions), "void", {kMesh, "f32[]"}, error) &&
        fn(P("setNormals"), F(&setNormals), "void", {kMesh, "f32[]"}, error) &&
        fn(P("setUVs"), F(&setUVs), "void", {kMesh, "f32[]"}, error) &&
        fn(P("setColors"), F(&setColors), "void", {kMesh, "f32[]"}, error) &&
        fn(P("setIndices"), F(&setIndices), "void", {kMesh, "u32[]"}, error) &&
        fn(P("clone"), F(&clone), kMesh, {kMesh}, error) &&
        fn(P("translate"), F(&translate), "void", {kMesh, "f64", "f64", "f64"}, error) &&
        fn(P("scale"), F(&scale), "void", {kMesh, "f64", "f64", "f64"}, error) &&
        fn(P("rotate"), F(&rotate), "void", {kMesh, "f64", "f64", "f64", "f64"}, error) &&
        fn(P("center"), F(&center), "void", {kMesh}, error) &&
        fn(P("fitToBox"), F(&fitToBox), "void", {kMesh, "f64"}, error) &&
        fn(P("transform"), F(&transform), "void", {kMesh, "f32[]"}, error) &&
        fn(P("computeNormals"), F(&computeNormals), "void", {kMesh, "f64"}, error) &&
        fn(P("invertNormals"), F(&invertNormals), "void", {kMesh}, error) &&
        fn(P("flipFaces"), F(&flipFaces), "void", {kMesh}, error) &&
        fn(P("weld"), F(&weld), "void", {kMesh, "f64"}, error) &&
        fn(P("simplify"), F(&simplify), "void", {kMesh, "f64", "f64"}, error) &&
        fn(P("subdivideLoop"), F(&subdivideLoop), "void", {kMesh, "i32"}, error) &&
        fn(P("subdivideCatmullClark"), F(&subdivideCatmullClark), "void", {kMesh, "i32"}, error) &&
        fn(P("smooth"), F(&smooth), "void", {kMesh, "f64", "i32"}, error) &&
        fn(P("remesh"), F(&remesh), "void", {kMesh, "f64"}, error) &&
        fn(P("repair"), F(&repair), "void", {kMesh}, error) &&
        fn(P("optimize"), F(&optimize), "void", {kMesh}, error) &&
        fn(P("splitComponents"), F(&splitComponents), "i32", {kMesh}, error) &&
        fn(P("convexDecomposition"), F(&convexDecomposition), "i32",
           {kMesh, "i32", "i32", "f64", "f64"}, error) &&
        fn(P("takePiece"), F(&takePiece), kMesh, {"i32"}, error) &&
        fn(P("convexHull"), F(&convexHull), kMesh, {kMesh}, error) &&
        fn(P("booleanUnion"), F(&booleanUnion), kMesh, {kMesh, kMesh}, error) &&
        fn(P("booleanDifference"), F(&booleanDifference), kMesh, {kMesh, kMesh}, error) &&
        fn(P("booleanIntersection"), F(&booleanIntersection), kMesh, {kMesh, kMesh}, error) &&
        fn(P("unwrapUVs"), F(&unwrapUVs), "bool", {kMesh}, error) &&
        fn(P("projectUVs"), F(&projectUVs), "void", {kMesh, "i32", "f64"}, error) &&
        fn(P("computeUVDistortion"), F(&computeUVDistortion), "f32[]", {kMesh}, error) &&
        fn(P("measureUVQuality"), F(&measureUVQuality), "str", {kMesh}, error) &&
        fn(P("buildMeshlets"), F(&buildMeshlets), "i32", {kMesh, "i32", "i32", "f64"}, error) &&
        fn(P("meshletVertices"), F(&meshletVertices), "u32[]", {}, error) &&
        fn(P("meshletTriangles"), F(&meshletTriangles), "u8[]", {}, error) &&
        fn(P("meshletRecords"), F(&meshletRecords), "u8[]", {}, error) &&
        fn(P("bounds"), F(&bounds), "f32[]", {kMesh}, error) &&
        fn(P("surfaceArea"), F(&surfaceArea), "f64", {kMesh}, error) &&
        fn(P("volume"), F(&volume), "f64", {kMesh}, error) &&
        fn(P("isManifold"), F(&isManifold), "bool", {kMesh}, error) &&
        fn(P("triangleAreas"), F(&triangleAreas), "f32[]", {kMesh}, error) &&
        fn(P("raycast"), F(&raycast), "str",
           {kMesh, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("box"), F(&box), kMesh, {"f64", "f64", "f64"}, error) &&
        fn(P("sphere"), F(&sphere), kMesh, {"f64", "i32", "i32"}, error) &&
        fn(P("cylinder"), F(&cylinder), kMesh, {"f64", "f64", "i32"}, error) &&
        fn(P("capsule"), F(&capsule), kMesh, {"f64", "f64", "i32", "i32"}, error) &&
        fn(P("cone"), F(&cone), kMesh, {"f64", "f64", "i32", "i32"}, error) &&
        fn(P("plane"), F(&plane), kMesh, {"f64", "f64", "i32", "i32"}, error) &&
        fn(P("torus"), F(&torus), kMesh, {"f64", "f64", "i32", "i32"}, error) &&
        fn(P("icosahedron"), F(&icosahedron), kMesh, {"f64"}, error) &&
        fn(P("dodecahedron"), F(&dodecahedron), kMesh, {"f64"}, error) &&
        fn(P("octahedron"), F(&octahedron), kMesh, {"f64"}, error) &&
        fn(P("tetrahedron"), F(&tetrahedron), kMesh, {"f64"}, error) &&
        fn(P("disk"), F(&disk), kMesh, {"f64", "i32"}, error) &&
        fn(P("geodesicSphere"), F(&geodesicSphere), kMesh, {"f64", "i32"}, error) &&
        fn(P("rock"), F(&rock), kMesh, {"f64", "i32", "i32"}, error) &&
        fn(P("blob"), F(&blob), kMesh,
           {"f64", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("tube"), F(&tube), kMesh, {"f32[]", "f64", "i32"}, error) &&
        fn(P("mergeBegin"), F(&mergeBegin), "void", {}, error) &&
        fn(P("mergeAdd"), F(&mergeAdd), "void", {kMesh}, error) &&
        fn(P("mergeEnd"), F(&mergeEnd), kMesh, {}, error) &&
        fn(P("marchingCubes"), F(&marchingCubes), kMesh, {"f32[]", "i32", "i32", "i32", "f64"},
           error) &&
        fn(P("surfaceNets"), F(&surfaceNets), kMesh, {"f32[]", "i32", "i32", "i32", "f64"}, error) &&
        fn(P("dualContouring"), F(&dualContouring), kMesh, {"f32[]", "i32", "i32", "i32", "f64"},
           error) &&
#if BROMESH_HAS_DRACO
        fn(P("dracoDecode"), F(&dracoDecode), "str", {"u8[]"}, error) &&
        fn(P("dracoTakeMesh"), F(&dracoTakeMesh), kMesh, {}, error) &&
        fn(P("dracoAttributeCount"), F(&dracoAttributeCount), "i32", {}, error) &&
        fn(P("dracoAttributeInfo"), F(&dracoAttributeInfo), "str", {"i32"}, error) &&
        fn(P("dracoAttributeBytes"), F(&dracoAttributeBytes), "u8[]", {"i32"}, error) &&
        fn(P("dracoEncode"), F(&dracoEncode), "u8[]", {kMesh, "i32", "i32", "i32", "i32", "i32"},
           error) &&
#endif
        ctor(kBVH, F(&bvhNew), &bvhFree, {kMesh, "i32"}, error) &&
        fn(P("bvhRaycast"), F(&bvhRaycast), "str",
           {kBVH, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("bvhRaycastTest"), F(&bvhRaycastTest), "bool",
           {kBVH, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("bvhEmpty"), F(&bvhEmpty), "bool", {kBVH}, error) &&
        fn(P("bvhNodeCount"), F(&bvhNodeCount), "f64", {kBVH}, error) &&
        fn(P("bvhTriangleCount"), F(&bvhTriangleCount), "f64", {kBVH}, error) &&
        fn(P("bvhBounds"), F(&bvhBounds), "f32[]", {kBVH}, error);
    return ok;
}

void publishMeshPrototypes(Value nativeRoot) {
    // The prototypes the two classes' handles are born on, as properties of
    // the plain `__bro_native.mesh` object, so js/mesh.js can chain them
    // under its public Mesh / MeshBVH classes (one dynamic read each).
    ev::Persistent root(nativeRoot);
    ev::Persistent ns(ev::getProperty(root.get(), "mesh"));
    ev::GlobalValue meshProto = ev::nativeClassPrototype(kMesh);
    if (meshProto.found) {
        ev::Persistent p(meshProto.value);
        ev::setProperty(ns.get(), "MeshPrototype", p.get());
    }
    ev::GlobalValue bvhProto = ev::nativeClassPrototype(kBVH);
    if (bvhProto.found) {
        ev::Persistent p(bvhProto.value);
        ev::setProperty(ns.get(), "MeshBVHPrototype", p.get());
    }
}

}  // namespace bro::bronze_host

#else  // !BRO_WITH_3D

namespace bro::bronze_host {
// bromesh is not in this build; nothing under __bro_native.mesh exists and
// js/mesh.js is not installed (dom_globals.cpp).
bool registerMeshNatives(std::string*) { return true; }
void publishMeshPrototypes(Value) {}
}  // namespace bro::bronze_host

#endif
