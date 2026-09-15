#include "native_mesh_internal.h"

#if BRO_WITH_3D

namespace bro::bronze_host {

namespace {

thread_local std::vector<MeshData> g_pieces;
thread_local std::vector<MeshData> g_lodChain;
thread_local std::pair<MeshData, MeshData> g_splitPair;
thread_local std::vector<MeshData> g_mergeList;
thread_local bromesh::TextureBuffer g_texBuffer;

}  // namespace

// ---- In-place transforms ---------------------------------------------------

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

void center(void* m) {
    bromesh::centerMesh(M(m));
}

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

void mirror(void* m, int32_t axis) {
    bromesh::mirrorMesh(M(m), axis);
}

// ---- Normals & Repair ------------------------------------------------------

void computeNormals(void* m, double creaseAngleDeg) {
    if (creaseAngleDeg > 0.0) {
        M(m) = bromesh::computeCreaseNormals(M(m), static_cast<float>(creaseAngleDeg));
    } else {
        bromesh::computeNormals(M(m));
    }
}

void* computeFlatNormals(void* m) {
    return take(bromesh::computeFlatNormals(M(m)));
}

void computeCreaseNormals(void* m, double angle) {
    M(m) = bromesh::computeCreaseNormals(M(m), static_cast<float>(angle > 0.0 ? angle : 30.0f));
}

void invertNormals(void* m) {
    for (float& f : M(m).normals) f = -f;
}

void flipFaces(void* m) {
    auto& idx = M(m).indices;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
}

void weld(void* m, double eps) {
    M(m) = bromesh::weldVertices(M(m), static_cast<float>(eps));
}

void removeDegenerateTriangles(void* m, double eps) {
    M(m) = bromesh::removeDegenerateTriangles(M(m), static_cast<float>(eps > 0.0 ? eps : 1e-8f));
}

void removeDuplicateTriangles(void* m) {
    M(m) = bromesh::removeDuplicateTriangles(M(m));
}

void fillHoles(void* m, int32_t maxEdges) {
    M(m) = bromesh::fillHoles(M(m), maxEdges > 0 ? maxEdges : 64);
}

void repair(void* m) {
    MeshData& mesh = M(m);
    mesh = bromesh::removeDegenerateTriangles(mesh);
    mesh = bromesh::removeDuplicateTriangles(mesh);
    mesh = bromesh::fillHoles(mesh);
}

// ---- Simplification, Subdivision, Smoothing, Remeshing ---------------------

void simplify(void* m, double ratio, double targetError) {
    M(m) = bromesh::simplify(M(m), static_cast<float>(ratio), static_cast<float>(targetError));
}

void simplifyToTriangleCount(void* m, int32_t count, double targetError) {
    if (count <= 0) return;
    M(m) = bromesh::simplifyToTriangleCount(M(m), static_cast<size_t>(count),
                                            static_cast<float>(targetError > 0.0 ? targetError : 0.01f));
}

int32_t generateLODChain(void* m, const float* ratios, uint32_t rn) {
    if (!ratios || rn == 0) {
        g_lodChain.clear();
        return 0;
    }
    g_lodChain = bromesh::generateLODChain(M(m), ratios, static_cast<int>(rn));
    return static_cast<int32_t>(g_lodChain.size());
}

void* takeLOD(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= g_lodChain.size()) {
        ev::throwRangeError("LOD piece " + std::to_string(i) + " is not in the snapshot");
        return nullptr;
    }
    return take(std::move(g_lodChain[static_cast<size_t>(i)]));
}

void subdivideLoop(void* m, int32_t iterations) {
    M(m) = bromesh::subdivideLoop(M(m), iterations);
}

void subdivideCatmullClark(void* m, int32_t iterations) {
    M(m) = bromesh::subdivideCatmullClark(M(m), iterations);
}

void smooth(void* m, double lambda, int32_t iterations) {
    bromesh::smoothLaplacian(M(m), static_cast<float>(lambda), iterations);
}

void smoothLaplacian(void* m, double lambda, int32_t iterations) {
    bromesh::smoothLaplacian(M(m), static_cast<float>(lambda), iterations);
}

void smoothTaubin(void* m, double lambda, double mu, int32_t iterations) {
    bromesh::smoothTaubin(M(m), static_cast<float>(lambda), static_cast<float>(mu), iterations);
}

void remesh(void* m, double targetEdgeLength) {
    M(m) = bromesh::remeshIsotropic(M(m), static_cast<float>(targetEdgeLength));
}

void remeshIsotropic(void* m, double targetEdgeLen, int32_t iters) {
    M(m) = bromesh::remeshIsotropic(M(m), static_cast<float>(targetEdgeLen), iters > 0 ? iters : 5);
}

// ---- Shrinkwrap, Split, Merge, Components, CSG -----------------------------

void shrinkwrap(void* m, void* target, int32_t mode, double maxDist, double offset,
                const float* axis, uint32_t an) {
    auto swMode = bromesh::ShrinkwrapMode::Nearest;
    if (mode == 1) swMode = bromesh::ShrinkwrapMode::ProjectAlongNormal;
    else if (mode == 2) swMode = bromesh::ShrinkwrapMode::ProjectAlongAxis;

    const float* ax = (axis && an >= 3) ? axis : nullptr;
    bromesh::shrinkwrap(M(m), M(target), swMode, static_cast<float>(maxDist),
                        static_cast<float>(offset), ax);
}

void splitByPlane(void* m, double nx, double ny, double nz, double d) {
    g_splitPair = bromesh::splitByPlane(M(m), static_cast<float>(nx), static_cast<float>(ny),
                                        static_cast<float>(nz), static_cast<float>(d));
}

void* takeSplit(int32_t which) {
    if (which == 0) return take(std::move(g_splitPair.first));
    if (which == 1) return take(std::move(g_splitPair.second));
    return nullptr;
}

int32_t splitComponents(void* m) {
    g_pieces = bromesh::splitConnectedComponents(M(m));
    return static_cast<int32_t>(g_pieces.size());
}

int32_t convexDecomposition(void* m, int32_t maxHulls, int32_t maxVerticesPerHull,
                            double resolution, double minVolumePerHull) {
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

void* convexHull(void* m) {
    return take(bromesh::convexHull(M(m)));
}

void* booleanUnion(void* a, void* b) {
    return take(bromesh::booleanUnion(M(a), M(b)));
}

void* booleanDifference(void* a, void* b) {
    return take(bromesh::booleanDifference(M(a), M(b)));
}

void* booleanIntersection(void* a, void* b) {
    return take(bromesh::booleanIntersection(M(a), M(b)));
}

void mergeBegin() {
    g_mergeList.clear();
}

void mergeAdd(void* m) {
    g_mergeList.push_back(M(m));
}

void* mergeEnd() {
    MeshData merged = bromesh::mergeMeshes(g_mergeList);
    g_mergeList.clear();
    return take(std::move(merged));
}

// ---- Baking ----------------------------------------------------------------

void bakeAmbientOcclusion(void* m, int32_t numRays, double maxDist) {
    bromesh::bakeAmbientOcclusion(M(m), numRays > 0 ? numRays : 64, static_cast<float>(maxDist));
}

void bakeCurvature(void* m, double scale) {
    bromesh::bakeCurvature(M(m), static_cast<float>(scale > 0.0 ? scale : 1.0f));
}

void bakeThickness(void* m, int32_t numRays, double maxDist) {
    bromesh::bakeThickness(M(m), numRays > 0 ? numRays : 32, static_cast<float>(maxDist));
}

void bakeAOToTexture(void* m, int32_t w, int32_t h, int32_t numRays, double maxDist) {
    g_texBuffer = bromesh::bakeAmbientOcclusionToTexture(
        M(m), w > 0 ? w : 32, h > 0 ? h : 32,
        numRays > 0 ? numRays : 64, static_cast<float>(maxDist));
}

void bakeCurvatureToTexture(void* m, int32_t w, int32_t h, double scale) {
    g_texBuffer = bromesh::bakeCurvatureToTexture(
        M(m), w > 0 ? w : 32, h > 0 ? h : 32,
        static_cast<float>(scale > 0.0 ? scale : 1.0f));
}

void bakeThicknessToTexture(void* m, int32_t w, int32_t h, int32_t numRays, double maxDist) {
    g_texBuffer = bromesh::bakeThicknessToTexture(
        M(m), w > 0 ? w : 32, h > 0 ? h : 32,
        numRays > 0 ? numRays : 32, static_cast<float>(maxDist));
}

void bakeNormalsToTexture(void* m, int32_t w, int32_t h) {
    g_texBuffer = bromesh::bakeNormalsToTexture(M(m), w > 0 ? w : 32, h > 0 ? h : 32);
}

void bakePositionToTexture(void* m, int32_t w, int32_t h) {
    g_texBuffer = bromesh::bakePositionToTexture(M(m), w > 0 ? w : 32, h > 0 ? h : 32);
}

void bakeNormalsFromReference(void* low, void* high, int32_t w, int32_t h, double searchDist) {
    g_texBuffer = bromesh::bakeNormalsFromReference(
        M(low), M(high), w > 0 ? w : 32, h > 0 ? h : 32,
        static_cast<float>(searchDist));
}

void bakeAOFromReference(void* low, void* high, int32_t w, int32_t h, int32_t numRays, double maxDist) {
    g_texBuffer = bromesh::bakeAOFromReference(
        M(low), M(high), w > 0 ? w : 32, h > 0 ? h : 32,
        numRays > 0 ? numRays : 64, static_cast<float>(maxDist));
}

int32_t texBufferWidth() {
    return g_texBuffer.width;
}

int32_t texBufferHeight() {
    return g_texBuffer.height;
}

int32_t texBufferChannels() {
    return g_texBuffer.channels;
}

void texBufferPixels(bronze_native_buffer* out) {
    transferOut(std::move(g_texBuffer.pixels), out);
}

}  // namespace bro::bronze_host

#endif // BRO_WITH_3D
