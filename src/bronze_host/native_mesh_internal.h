#pragma once

// Internal declarations and shared helpers for the bronze host mesh subsystem.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"

#if BRO_WITH_3D

#include <bromesh/analysis/bake.h>
#include <bromesh/analysis/bake_texture.h>
#include <bromesh/analysis/bake_transfer.h>
#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/convex_decomposition.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/sample.h>
#include <bromesh/csg/boolean.h>
#include <bromesh/isosurface/dual_contouring.h>
#include <bromesh/isosurface/marching_cubes.h>
#include <bromesh/isosurface/surface_nets.h>
#include <bromesh/isosurface/transvoxel.h>
#include <bromesh/manipulation/merge.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/remesh.h>
#include <bromesh/manipulation/repair.h>
#include <bromesh/manipulation/shrinkwrap.h>
#include <bromesh/manipulation/simplify.h>
#include <bromesh/manipulation/smooth.h>
#include <bromesh/manipulation/split_components.h>
#include <bromesh/manipulation/subdivide.h>
#include <bromesh/manipulation/sweep.h>
#include <bromesh/manipulation/transform.h>
#include <bromesh/manipulation/weld.h>
#include <bromesh/mesh_data.h>
#include <bromesh/optimization/analyze.h>
#include <bromesh/optimization/encode.h>
#include <bromesh/optimization/meshlets.h>
#include <bromesh/optimization/optimize.h>
#include <bromesh/optimization/progressive.h>
#include <bromesh/optimization/spatial.h>
#include <bromesh/optimization/strips.h>
#include <bromesh/primitives/par_primitives.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/uv/projection.h>
#include <bromesh/uv/unwrap.h>
#include <bromesh/uv/uv_metrics.h>
#include <bromesh/voxel/greedy_mesh.h>
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

using bromesh::MeshData;

inline constexpr const char* kMesh = "__bro_native.mesh.Mesh";
inline constexpr const char* kBVH = "__bro_native.mesh.MeshBVH";
inline constexpr const char* kProgressiveMesh = "__bro_native.mesh.ProgressiveMesh";

inline MeshData& M(void* handle) { return *static_cast<MeshData*>(handle); }
inline void* take(MeshData&& m) { return new MeshData(std::move(m)); }
inline void meshFree(void* p) { delete static_cast<MeshData*>(p); }

template <typename T>
inline void copyOut(const std::vector<T>& v, bronze_native_buffer* out) {
    out->data = v.empty() ? nullptr : const_cast<T*>(v.data());
    out->length = static_cast<uint32_t>(v.size());
}

template <typename T>
inline void releaseVector(void* ctx) { delete static_cast<std::vector<T>*>(ctx); }

template <typename T>
inline void transferOut(std::vector<T>&& v, bronze_native_buffer* out) {
    if (v.empty()) return;
    auto* owned = new std::vector<T>(std::move(v));
    out->data = owned->data();
    out->length = static_cast<uint32_t>(owned->size());
    out->release = &releaseVector<T>;
    out->ctx = owned;
}

inline std::string num(double d) {
    if (!std::isfinite(d)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", d);
    return buf;
}

inline std::string triple(const float* v) {
    return "[" + num(v[0]) + "," + num(v[1]) + "," + num(v[2]) + "]";
}

inline std::string hitJson(const MeshData& m, const bromesh::RayHit& h) {
    if (!h.hit) return std::string();
    std::string s = "{\"hit\":true,\"distance\":" + num(h.distance) +
                    ",\"triangle\":" + std::to_string(h.triangleIndex) +
                    ",\"triangleIndex\":" + std::to_string(h.triangleIndex) +
                    ",\"point\":" + triple(h.position) +
                    ",\"position\":" + triple(h.position) +
                    ",\"normal\":" + triple(h.normal) +
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

// ---- Operations (native_mesh_ops.cpp) ----
void translate(void* m, double dx, double dy, double dz);
void scale(void* m, double sx, double sy, double sz);
void rotate(void* m, double ax, double ay, double az, double angle);
void center(void* m);
void fitToBox(void* m, double size);
void transform(void* m, const float* mat, uint32_t n);
void mirror(void* m, int32_t axis);
void computeNormals(void* m, double creaseAngleDeg);
void* computeFlatNormals(void* m);
void computeCreaseNormals(void* m, double angle);
void invertNormals(void* m);
void flipFaces(void* m);
void weld(void* m, double eps);
void removeDegenerateTriangles(void* m, double eps);
void removeDuplicateTriangles(void* m);
void fillHoles(void* m, int32_t maxEdges);
void simplify(void* m, double ratio, double targetError);
void simplifyToTriangleCount(void* m, int32_t count, double targetError);
int32_t generateLODChain(void* m, const float* ratios, uint32_t rn);
void* takeLOD(int32_t i);
void subdivideLoop(void* m, int32_t iterations);
void subdivideCatmullClark(void* m, int32_t iterations);
void smooth(void* m, double lambda, int32_t iterations);
void smoothLaplacian(void* m, double lambda, int32_t iterations);
void smoothTaubin(void* m, double lambda, double mu, int32_t iterations);
void remesh(void* m, double targetEdgeLength);
void remeshIsotropic(void* m, double targetEdgeLen, int32_t iters);
void repair(void* m);
void shrinkwrap(void* m, void* target, int32_t mode, double maxDist, double offset,
                const float* axis, uint32_t an);
void splitByPlane(void* m, double nx, double ny, double nz, double d);
void* takeSplit(int32_t which);
int32_t splitComponents(void* m);
int32_t convexDecomposition(void* m, int32_t maxHulls, int32_t maxVerticesPerHull,
                            double resolution, double minVolumePerHull);
void* takePiece(int32_t i);
void* convexHull(void* m);
void* booleanUnion(void* a, void* b);
void* booleanDifference(void* a, void* b);
void* booleanIntersection(void* a, void* b);
void mergeBegin();
void mergeAdd(void* m);
void* mergeEnd();

// Baking (native_mesh_ops.cpp)
void bakeAmbientOcclusion(void* m, int32_t numRays, double maxDist);
void bakeCurvature(void* m, double scale);
void bakeThickness(void* m, int32_t numRays, double maxDist);
void bakeAOToTexture(void* m, int32_t w, int32_t h, int32_t numRays, double maxDist);
void bakeCurvatureToTexture(void* m, int32_t w, int32_t h, double scale);
void bakeThicknessToTexture(void* m, int32_t w, int32_t h, int32_t numRays, double maxDist);
void bakeNormalsToTexture(void* m, int32_t w, int32_t h);
void bakePositionToTexture(void* m, int32_t w, int32_t h);
void bakeNormalsFromReference(void* low, void* high, int32_t w, int32_t h, double searchDist);
void bakeAOFromReference(void* low, void* high, int32_t w, int32_t h, int32_t numRays, double maxDist);
int32_t texBufferWidth();
int32_t texBufferHeight();
int32_t texBufferChannels();
void texBufferPixels(bronze_native_buffer* out);

// ---- Analysis & BVH & Optim & Isosurfaces (native_mesh_analysis.cpp) ----
void bounds(void* m, bronze_native_buffer* out);
double surfaceArea(void* m);
double volume(void* m);
bool isManifold(void* m);
void triangleAreas(void* m, bronze_native_buffer* out);
void* sampleSurface(void* m, int32_t count, int32_t seed);
const char* raycast(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                    double maxDist);
const char* raycastAll(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                       double maxDist);
bool raycastTest(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                 double maxDist);
const char* closestPoint(void* m, double px, double py, double pz);

// BVH
void* bvhNew(void* mesh, int32_t leafSize);
void bvhFree(void* p);
const char* bvhRaycast(void* b, double ox, double oy, double oz, double dx, double dy, double dz,
                       double maxDist);
bool bvhRaycastTest(void* b, double ox, double oy, double oz, double dx, double dy, double dz,
                    double maxDist);
const char* bvhClosestPoint(void* b, double px, double py, double pz);
bool bvhEmpty(void* b);
double bvhNodeCount(void* b);
double bvhTriangleCount(void* b);
void bvhBounds(void* b, bronze_native_buffer* out);

// UVs
const char* unwrapUVs(void* m);
void projectUVs(void* m, int32_t type, double scale);
void computeUVDistortion(void* m, bronze_native_buffer* out);
const char* measureUVQuality(void* m);

// Meshlets
int32_t buildMeshlets(void* m, int32_t maxVertices, int32_t maxTriangles, double coneWeight);
void meshletVertices(bronze_native_buffer* out);
void meshletTriangles(bronze_native_buffer* out);
void meshletRecords(bronze_native_buffer* out);

// Optimization
const char* analyzeVertexCache(void* m, int32_t cacheSize);
const char* analyzeVertexFetch(void* m, int32_t vertexSize);
const char* analyzeOverdraw(void* m);
void optimize(void* m);
void optimizeVertexCache(void* m);
void optimizeVertexFetch(void* m);
void optimizeOverdraw(void* m, double threshold);
void spatialSortTriangles(void* m);
void spatialSortVertices(void* m);
void generateShadowIndexBuffer(void* m, bronze_native_buffer* out);
void stripify(const uint32_t* indices, uint32_t in, int32_t vertexCount, bronze_native_buffer* out);
void unstripify(const uint32_t* strip, uint32_t sn, bronze_native_buffer* out);
void encodeMesh(void* m);
void encodedVertexData(bronze_native_buffer* out);
void encodedIndexData(bronze_native_buffer* out);
double encodedVertexCount();
double encodedVertexSize();
double encodedIndexCount();
void* decodeMesh(const uint8_t* vdata, uint32_t vn, const uint8_t* idata, uint32_t in,
                int32_t vertexCount, int32_t vertexSize, int32_t indexCount,
                bool hasNormals, bool hasUVs, bool hasColors);

// ProgressiveMesh
void* pmNew(void* mesh);
void pmFree(void* p);
double pmMaxTriangles(void* pm);
double pmMinTriangles(void* pm);
void* pmAtRatio(void* pm, double ratio);
void* pmAtTriangleCount(void* pm, int32_t count);
void pmSerialize(void* pm, bronze_native_buffer* out);
void* pmDeserialize(const uint8_t* data, uint32_t n);

// Isosurfaces & voxels
void* marchingCubes(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso);
void* surfaceNets(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso);
void* dualContouring(const float* field, uint32_t n, int32_t gx, int32_t gy, int32_t gz, double iso);
void* transvoxel(const float* field, uint32_t fn, int32_t gridSize, int32_t lod,
                 const int32_t* neighborLods, uint32_t nn, double isoLevel, double cellSize);
void* greedyMesh(const uint8_t* voxels, uint32_t vn, int32_t gx, int32_t gy, int32_t gz,
                 double cellSize);

// Draco
#if BROMESH_HAS_DRACO
const char* dracoDecode(const uint8_t* bytes, uint32_t n);
void* dracoTakeMesh();
int32_t dracoAttributeCount();
const char* dracoAttributeInfo(int32_t i);
void dracoAttributeBytes(int32_t i, bronze_native_buffer* out);
void dracoEncode(void* m, int32_t positionBits, int32_t normalBits, int32_t uvBits, int32_t colorBits,
                 int32_t speed, bronze_native_buffer* out);
#endif

}  // namespace bro::bronze_host

#endif // BRO_WITH_3D
