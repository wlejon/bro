#include "native_mesh_internal.h"

#if BRO_WITH_3D

namespace bro::bronze_host {

namespace {

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

void positions(void* m, bronze_native_buffer* out) { copyOut(M(m).positions, out); }
void normals(void* m, bronze_native_buffer* out) { copyOut(M(m).normals, out); }
void uvs(void* m, bronze_native_buffer* out) { copyOut(M(m).uvs, out); }
void colors(void* m, bronze_native_buffer* out) { copyOut(M(m).colors, out); }
void indices(void* m, bronze_native_buffer* out) { copyOut(M(m).indices, out); }

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

// ---- Primitives ------------------------------------------------------------

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
void* heightmapGrid(const float* heights, uint32_t hn, int32_t gw, int32_t gh, double cellSize, int32_t border) {
    if (gw <= 0 || gh <= 0) {
        ev::throwTypeError("Mesh.heightmapGrid: width and depth must be positive");
        return nullptr;
    }
    int b = border > 0 ? border : 0;
    size_t expected = static_cast<size_t>(gw + 2 * b) * (gh + 2 * b);
    if (hn < expected) {
        ev::throwTypeError("Mesh.heightmapGrid: heights array too short");
        return nullptr;
    }
    return take(bromesh::heightmapGrid(heights, gw, gh, static_cast<float>(cellSize > 0.0 ? cellSize : 1.0f), b));
}

}  // namespace

bool registerMeshNatives(std::string* error) {
    using namespace natives;
    auto F = [](auto* f) { return reinterpret_cast<void*>(f); };
    const char* ns = "__bro_native.mesh.";
    auto P = [&](const char* name) {
        thread_local std::deque<std::string> keep;
        keep.push_back(std::string(ns) + name);
        return keep.back().c_str();
    };

    bool ok =
        // Classes
        ctor(kMesh, F(&meshNew), &meshFree, {"f32[]", "f32[]", "f32[]", "f32[]", "u32[]"}, error) &&
        ctor(kBVH, F(&bvhNew), &bvhFree, {kMesh, "i32"}, error) &&
        ctor(kProgressiveMesh, F(&pmNew), &pmFree, {kMesh}, error) &&

        // Mesh core accessors
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

        // Mesh transforms & manipulation
        fn(P("translate"), F(&translate), "void", {kMesh, "f64", "f64", "f64"}, error) &&
        fn(P("scale"), F(&scale), "void", {kMesh, "f64", "f64", "f64"}, error) &&
        fn(P("rotate"), F(&rotate), "void", {kMesh, "f64", "f64", "f64", "f64"}, error) &&
        fn(P("center"), F(&center), "void", {kMesh}, error) &&
        fn(P("fitToBox"), F(&fitToBox), "void", {kMesh, "f64"}, error) &&
        fn(P("transform"), F(&transform), "void", {kMesh, "f32[]"}, error) &&
        fn(P("mirror"), F(&mirror), "void", {kMesh, "i32"}, error) &&
        fn(P("computeNormals"), F(&computeNormals), "void", {kMesh, "f64"}, error) &&
        fn(P("computeFlatNormals"), F(&computeFlatNormals), kMesh, {kMesh}, error) &&
        fn(P("computeCreaseNormals"), F(&computeCreaseNormals), "void", {kMesh, "f64"}, error) &&
        fn(P("invertNormals"), F(&invertNormals), "void", {kMesh}, error) &&
        fn(P("flipFaces"), F(&flipFaces), "void", {kMesh}, error) &&
        fn(P("weld"), F(&weld), "void", {kMesh, "f64"}, error) &&
        fn(P("removeDegenerateTriangles"), F(&removeDegenerateTriangles), "void", {kMesh, "f64"}, error) &&
        fn(P("removeDuplicateTriangles"), F(&removeDuplicateTriangles), "void", {kMesh}, error) &&
        fn(P("fillHoles"), F(&fillHoles), "void", {kMesh, "i32"}, error) &&
        fn(P("simplify"), F(&simplify), "void", {kMesh, "f64", "f64"}, error) &&
        fn(P("simplifyToTriangleCount"), F(&simplifyToTriangleCount), "void", {kMesh, "i32", "f64"}, error) &&
        fn(P("generateLODChain"), F(&generateLODChain), "i32", {kMesh, "f32[]"}, error) &&
        fn(P("takeLOD"), F(&takeLOD), kMesh, {"i32"}, error) &&
        fn(P("subdivideLoop"), F(&subdivideLoop), "void", {kMesh, "i32"}, error) &&
        fn(P("subdivideCatmullClark"), F(&subdivideCatmullClark), "void", {kMesh, "i32"}, error) &&
        fn(P("smooth"), F(&smooth), "void", {kMesh, "f64", "i32"}, error) &&
        fn(P("smoothLaplacian"), F(&smoothLaplacian), "void", {kMesh, "f64", "i32"}, error) &&
        fn(P("smoothTaubin"), F(&smoothTaubin), "void", {kMesh, "f64", "f64", "i32"}, error) &&
        fn(P("remesh"), F(&remesh), "void", {kMesh, "f64"}, error) &&
        fn(P("remeshIsotropic"), F(&remeshIsotropic), "void", {kMesh, "f64", "i32"}, error) &&
        fn(P("repair"), F(&repair), "void", {kMesh}, error) &&
        fn(P("shrinkwrap"), F(&shrinkwrap), "void", {kMesh, kMesh, "i32", "f64", "f64", "f32[]"}, error) &&
        fn(P("splitByPlane"), F(&splitByPlane), "void", {kMesh, "f64", "f64", "f64", "f64"}, error) &&
        fn(P("takeSplit"), F(&takeSplit), kMesh, {"i32"}, error) &&
        fn(P("splitComponents"), F(&splitComponents), "i32", {kMesh}, error) &&
        fn(P("convexDecomposition"), F(&convexDecomposition), "i32", {kMesh, "i32", "i32", "f64", "f64"}, error) &&
        fn(P("takePiece"), F(&takePiece), kMesh, {"i32"}, error) &&
        fn(P("convexHull"), F(&convexHull), kMesh, {kMesh}, error) &&
        fn(P("booleanUnion"), F(&booleanUnion), kMesh, {kMesh, kMesh}, error) &&
        fn(P("booleanDifference"), F(&booleanDifference), kMesh, {kMesh, kMesh}, error) &&
        fn(P("booleanIntersection"), F(&booleanIntersection), kMesh, {kMesh, kMesh}, error) &&
        fn(P("mergeBegin"), F(&mergeBegin), "void", {}, error) &&
        fn(P("mergeAdd"), F(&mergeAdd), "void", {kMesh}, error) &&
        fn(P("mergeEnd"), F(&mergeEnd), kMesh, {}, error) &&

        // Baking
        fn(P("bakeAmbientOcclusion"), F(&bakeAmbientOcclusion), "void", {kMesh, "i32", "f64"}, error) &&
        fn(P("bakeCurvature"), F(&bakeCurvature), "void", {kMesh, "f64"}, error) &&
        fn(P("bakeThickness"), F(&bakeThickness), "void", {kMesh, "i32", "f64"}, error) &&
        fn(P("bakeAOToTexture"), F(&bakeAOToTexture), "void", {kMesh, "i32", "i32", "i32", "f64"}, error) &&
        fn(P("bakeCurvatureToTexture"), F(&bakeCurvatureToTexture), "void", {kMesh, "i32", "i32", "f64"}, error) &&
        fn(P("bakeThicknessToTexture"), F(&bakeThicknessToTexture), "void", {kMesh, "i32", "i32", "i32", "f64"}, error) &&
        fn(P("bakeNormalsToTexture"), F(&bakeNormalsToTexture), "void", {kMesh, "i32", "i32"}, error) &&
        fn(P("bakePositionToTexture"), F(&bakePositionToTexture), "void", {kMesh, "i32", "i32"}, error) &&
        fn(P("bakeNormalsFromReference"), F(&bakeNormalsFromReference), "void", {kMesh, kMesh, "i32", "i32", "f64"}, error) &&
        fn(P("bakeAOFromReference"), F(&bakeAOFromReference), "void", {kMesh, kMesh, "i32", "i32", "i32", "f64"}, error) &&
        fn(P("texBufferWidth"), F(&texBufferWidth), "i32", {}, error) &&
        fn(P("texBufferHeight"), F(&texBufferHeight), "i32", {}, error) &&
        fn(P("texBufferChannels"), F(&texBufferChannels), "i32", {}, error) &&
        fn(P("texBufferPixels"), F(&texBufferPixels), "f32[]", {}, error) &&

        // Analysis
        fn(P("bounds"), F(&bounds), "f32[]", {kMesh}, error) &&
        fn(P("surfaceArea"), F(&surfaceArea), "f64", {kMesh}, error) &&
        fn(P("volume"), F(&volume), "f64", {kMesh}, error) &&
        fn(P("isManifold"), F(&isManifold), "bool", {kMesh}, error) &&
        fn(P("triangleAreas"), F(&triangleAreas), "f32[]", {kMesh}, error) &&
        fn(P("sampleSurface"), F(&sampleSurface), kMesh, {kMesh, "i32", "i32"}, error) &&
        fn(P("raycast"), F(&raycast), "str", {kMesh, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("raycastAll"), F(&raycastAll), "str", {kMesh, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("raycastTest"), F(&raycastTest), "bool", {kMesh, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("closestPoint"), F(&closestPoint), "str", {kMesh, "f64", "f64", "f64"}, error) &&

        // BVH
        fn(P("bvhRaycast"), F(&bvhRaycast), "str", {kBVH, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("bvhRaycastTest"), F(&bvhRaycastTest), "bool", {kBVH, "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("bvhClosestPoint"), F(&bvhClosestPoint), "str", {kBVH, "f64", "f64", "f64"}, error) &&
        fn(P("bvhEmpty"), F(&bvhEmpty), "bool", {kBVH}, error) &&
        fn(P("bvhNodeCount"), F(&bvhNodeCount), "f64", {kBVH}, error) &&
        fn(P("bvhTriangleCount"), F(&bvhTriangleCount), "f64", {kBVH}, error) &&
        fn(P("bvhBounds"), F(&bvhBounds), "f32[]", {kBVH}, error) &&

        // UVs
        fn(P("unwrapUVs"), F(&unwrapUVs), "str", {kMesh}, error) &&
        fn(P("projectUVs"), F(&projectUVs), "void", {kMesh, "i32", "f64"}, error) &&
        fn(P("computeUVDistortion"), F(&computeUVDistortion), "f32[]", {kMesh}, error) &&
        fn(P("measureUVQuality"), F(&measureUVQuality), "str", {kMesh}, error) &&

        // Meshlets
        fn(P("buildMeshlets"), F(&buildMeshlets), "i32", {kMesh, "i32", "i32", "f64"}, error) &&
        fn(P("meshletVertices"), F(&meshletVertices), "u32[]", {}, error) &&
        fn(P("meshletTriangles"), F(&meshletTriangles), "u8[]", {}, error) &&
        fn(P("meshletRecords"), F(&meshletRecords), "u8[]", {}, error) &&

        // Optimization
        fn(P("analyzeVertexCache"), F(&analyzeVertexCache), "str", {kMesh, "i32"}, error) &&
        fn(P("analyzeVertexFetch"), F(&analyzeVertexFetch), "str", {kMesh, "i32"}, error) &&
        fn(P("analyzeOverdraw"), F(&analyzeOverdraw), "str", {kMesh}, error) &&
        fn(P("optimize"), F(&optimize), "void", {kMesh}, error) &&
        fn(P("optimizeVertexCache"), F(&optimizeVertexCache), "void", {kMesh}, error) &&
        fn(P("optimizeVertexFetch"), F(&optimizeVertexFetch), "void", {kMesh}, error) &&
        fn(P("optimizeOverdraw"), F(&optimizeOverdraw), "void", {kMesh, "f64"}, error) &&
        fn(P("spatialSortTriangles"), F(&spatialSortTriangles), "void", {kMesh}, error) &&
        fn(P("spatialSortVertices"), F(&spatialSortVertices), "void", {kMesh}, error) &&
        fn(P("generateShadowIndexBuffer"), F(&generateShadowIndexBuffer), "u32[]", {kMesh}, error) &&
        fn(P("stripify"), F(&stripify), "u32[]", {"u32[]", "i32"}, error) &&
        fn(P("unstripify"), F(&unstripify), "u32[]", {"u32[]"}, error) &&
        fn(P("encodeMesh"), F(&encodeMesh), "void", {kMesh}, error) &&
        fn(P("encodedVertexData"), F(&encodedVertexData), "u8[]", {}, error) &&
        fn(P("encodedIndexData"), F(&encodedIndexData), "u8[]", {}, error) &&
        fn(P("encodedVertexCount"), F(&encodedVertexCount), "f64", {}, error) &&
        fn(P("encodedVertexSize"), F(&encodedVertexSize), "f64", {}, error) &&
        fn(P("encodedIndexCount"), F(&encodedIndexCount), "f64", {}, error) &&
        fn(P("decodeMesh"), F(&decodeMesh), kMesh, {"u8[]", "u8[]", "i32", "i32", "i32", "bool", "bool", "bool"}, error) &&

        // ProgressiveMesh
        fn(P("pmMaxTriangles"), F(&pmMaxTriangles), "f64", {kProgressiveMesh}, error) &&
        fn(P("pmMinTriangles"), F(&pmMinTriangles), "f64", {kProgressiveMesh}, error) &&
        fn(P("pmAtRatio"), F(&pmAtRatio), kMesh, {kProgressiveMesh, "f64"}, error) &&
        fn(P("pmAtTriangleCount"), F(&pmAtTriangleCount), kMesh, {kProgressiveMesh, "i32"}, error) &&
        fn(P("pmSerialize"), F(&pmSerialize), "u8[]", {kProgressiveMesh}, error) &&
        fn(P("pmDeserialize"), F(&pmDeserialize), kProgressiveMesh, {"u8[]"}, error) &&

        // Primitives
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
        fn(P("blob"), F(&blob), kMesh, {"f64", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn(P("tube"), F(&tube), kMesh, {"f32[]", "f64", "i32"}, error) &&
        fn(P("heightmapGrid"), F(&heightmapGrid), kMesh, {"f32[]", "i32", "i32", "f64", "i32"}, error) &&

        // Isosurfaces
        fn(P("marchingCubes"), F(&marchingCubes), kMesh, {"f32[]", "i32", "i32", "i32", "f64"}, error) &&
        fn(P("surfaceNets"), F(&surfaceNets), kMesh, {"f32[]", "i32", "i32", "i32", "f64"}, error) &&
        fn(P("dualContouring"), F(&dualContouring), kMesh, {"f32[]", "i32", "i32", "i32", "f64"}, error) &&
        fn(P("transvoxel"), F(&transvoxel), kMesh, {"f32[]", "i32", "i32", "i32[]", "f64", "f64"}, error) &&
        fn(P("greedyMesh"), F(&greedyMesh), kMesh, {"u8[]", "i32", "i32", "i32", "f64"}, error) &&

#if BROMESH_HAS_DRACO
        fn(P("dracoDecode"), F(&dracoDecode), "str", {"u8[]"}, error) &&
        fn(P("dracoTakeMesh"), F(&dracoTakeMesh), kMesh, {}, error) &&
        fn(P("dracoAttributeCount"), F(&dracoAttributeCount), "i32", {}, error) &&
        fn(P("dracoAttributeInfo"), F(&dracoAttributeInfo), "str", {"i32"}, error) &&
        fn(P("dracoAttributeBytes"), F(&dracoAttributeBytes), "u8[]", {"i32"}, error) &&
        fn(P("dracoEncode"), F(&dracoEncode), "u8[]", {kMesh, "i32", "i32", "i32", "i32", "i32"}, error) &&
#endif
        true;

    return ok;
}

void publishMeshPrototypes(Value nativeRoot) {
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
    ev::GlobalValue pmProto = ev::nativeClassPrototype(kProgressiveMesh);
    if (pmProto.found) {
        ev::Persistent p(pmProto.value);
        ev::setProperty(ns.get(), "ProgressiveMeshPrototype", p.get());
    }
}

}  // namespace bro::bronze_host

#else  // !BRO_WITH_3D

namespace bro::bronze_host {
bool registerMeshNatives(std::string*) { return true; }
void publishMeshPrototypes(Value) {}
}  // namespace bro::bronze_host

#endif
