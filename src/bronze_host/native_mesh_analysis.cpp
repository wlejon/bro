#include "native_mesh_internal.h"

#if BRO_WITH_3D

namespace bro::bronze_host {

namespace {

thread_local float g_bounds[6];
thread_local std::vector<uint32_t> g_meshletVertices;
thread_local std::vector<uint8_t> g_meshletTriangles;
thread_local std::vector<uint8_t> g_meshletRecords;
thread_local bromesh::EncodedMesh g_encodedMesh;

#if BROMESH_HAS_DRACO
thread_local bromesh::DracoDecoded g_draco;
#endif

struct BvhCell {
    MeshData mesh;  // its own copy: the index can never outlive what it indexes
    bromesh::MeshBVH bvh;
};

BvhCell& B(void* p) { return *static_cast<BvhCell*>(p); }

bool fieldFits(const char* what, uint32_t n, int32_t gx, int32_t gy, int32_t gz) {
    if (gx <= 0 || gy <= 0 || gz <= 0 ||
        static_cast<uint64_t>(n) != static_cast<uint64_t>(gx) * gy * gz) {
        ev::throwTypeError(std::string("Mesh.") + what + ": field length must be dimX*dimY*dimZ");
        return false;
    }
    return true;
}

}  // namespace

// ---- Surface Analysis ------------------------------------------------------

void bounds(void* m, bronze_native_buffer* out) {
    const bromath::AABB3 bb = bromesh::computeBBox(M(m));
    g_bounds[0] = bb.min.x; g_bounds[1] = bb.min.y; g_bounds[2] = bb.min.z;
    g_bounds[3] = bb.max.x; g_bounds[4] = bb.max.y; g_bounds[5] = bb.max.z;
    out->data = g_bounds;  // COPY: a per-thread scratch
    out->length = 6;
}

double surfaceArea(void* m) {
    return bromesh::computeSurfaceArea(M(m));
}

double volume(void* m) {
    return bromesh::computeVolume(M(m));
}

bool isManifold(void* m) {
    return bromesh::isManifold(M(m));
}

void triangleAreas(void* m, bronze_native_buffer* out) {
    transferOut(bromesh::computeTriangleAreas(M(m)), out);
}

void* sampleSurface(void* m, int32_t count, int32_t seed) {
    return take(bromesh::sampleSurface(M(m), static_cast<size_t>(count > 0 ? count : 0),
                                       static_cast<uint32_t>(seed)));
}

const char* raycast(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                    double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    return natives::strResult(hitJson(M(m), bromesh::raycast(M(m), o, d, static_cast<float>(maxDist))));
}

const char* raycastAll(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                       double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    auto hits = bromesh::raycastAll(M(m), o, d, static_cast<float>(maxDist));
    std::string json = "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) json += ",";
        json += hitJson(M(m), hits[i]);
    }
    json += "]";
    return natives::strResult(json);
}

bool raycastTest(void* m, double ox, double oy, double oz, double dx, double dy, double dz,
                 double maxDist) {
    const float o[3] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    const float d[3] = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    return bromesh::raycastTest(M(m), o, d, static_cast<float>(maxDist));
}

const char* closestPoint(void* m, double px, double py, double pz) {
    const float p[3] = {static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)};
    return natives::strResult(hitJson(M(m), bromesh::closestPoint(M(m), p)));
}

// ---- MeshBVH ---------------------------------------------------------------

void* bvhNew(void* mesh, int32_t leafSize) {
    auto* cell = new BvhCell{M(mesh), bromesh::MeshBVH()};
    cell->bvh = bromesh::MeshBVH::build(cell->mesh, leafSize > 0 ? leafSize : 8);
    return cell;
}

void bvhFree(void* p) {
    delete static_cast<BvhCell*>(p);
}

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

const char* bvhClosestPoint(void* b, double px, double py, double pz) {
    const float p[3] = {static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)};
    BvhCell& cell = B(b);
    return natives::strResult(hitJson(cell.mesh, cell.bvh.closestPoint(cell.mesh, p)));
}

bool bvhEmpty(void* b) {
    return B(b).bvh.empty();
}

double bvhNodeCount(void* b) {
    return static_cast<double>(B(b).bvh.nodeCount());
}

double bvhTriangleCount(void* b) {
    return static_cast<double>(B(b).bvh.triangleCount());
}

void bvhBounds(void* b, bronze_native_buffer* out) {
    const bromath::AABB3 bb = B(b).bvh.bounds();
    g_bounds[0] = bb.min.x; g_bounds[1] = bb.min.y; g_bounds[2] = bb.min.z;
    g_bounds[3] = bb.max.x; g_bounds[4] = bb.max.y; g_bounds[5] = bb.max.z;
    out->data = g_bounds;  // COPY
    out->length = 6;
}

// ---- UVs -------------------------------------------------------------------

const char* unwrapUVs(void* m) {
    bromesh::UnwrapParams up;
    bromesh::PackParams pp;
    const bromesh::UnwrapResult res = bromesh::unwrapUVs(M(m), up, pp);
    return natives::strResult(
        "{\"success\":" + std::string(res.success ? "true" : "false") +
        ",\"atlasWidth\":" + std::to_string(res.atlasWidth) +
        ",\"atlasHeight\":" + std::to_string(res.atlasHeight) +
        ",\"chartCount\":" + std::to_string(res.chartCount) + "}");
}

void projectUVs(void* m, int32_t type, double scale) {
    if (type < 0 || type > static_cast<int32_t>(bromesh::ProjectionType::Spherical)) {
        ev::throwRangeError("Mesh.projectUVs: unknown projection " + std::to_string(type));
        return;
    }
    bromesh::projectUVs(M(m), static_cast<bromesh::ProjectionType>(type),
                        static_cast<float>(scale > 0.0 ? scale : 1.0));
}

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

// ---- Meshlets --------------------------------------------------------------

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

// ---- Optimization ----------------------------------------------------------

const char* analyzeVertexCache(void* m, int32_t cacheSize) {
    auto st = bromesh::analyzeVertexCache(M(m), cacheSize > 0 ? static_cast<unsigned int>(cacheSize) : 16u);
    return natives::strResult(
        "{\"verticesTransformed\":" + std::to_string(st.verticesTransformed) +
        ",\"warpsExecuted\":" + std::to_string(st.warpsExecuted) +
        ",\"acmr\":" + num(st.acmr) +
        ",\"atvr\":" + num(st.atvr) + "}");
}

const char* analyzeVertexFetch(void* m, int32_t vertexSize) {
    auto st = bromesh::analyzeVertexFetch(M(m), vertexSize > 0 ? static_cast<size_t>(vertexSize) : 32);
    return natives::strResult(
        "{\"bytesFetched\":" + std::to_string(st.bytesFetched) +
        ",\"overfetch\":" + num(st.overfetch) + "}");
}

const char* analyzeOverdraw(void* m) {
    auto st = bromesh::analyzeOverdraw(M(m));
    return natives::strResult(
        "{\"pixelsCovered\":" + std::to_string(st.pixelsCovered) +
        ",\"pixelsShaded\":" + std::to_string(st.pixelsShaded) +
        ",\"overdraw\":" + num(st.overdraw) + "}");
}

void optimize(void* m) {
    bromesh::optimizeVertexCache(M(m));
    bromesh::optimizeOverdraw(M(m));
    bromesh::optimizeVertexFetch(M(m));
}

void optimizeVertexCache(void* m) {
    bromesh::optimizeVertexCache(M(m));
}

void optimizeVertexFetch(void* m) {
    bromesh::optimizeVertexFetch(M(m));
}

void optimizeOverdraw(void* m, double threshold) {
    bromesh::optimizeOverdraw(M(m), static_cast<float>(threshold > 0.0 ? threshold : 1.05f));
}

void spatialSortTriangles(void* m) {
    bromesh::spatialSortTriangles(M(m));
}

void spatialSortVertices(void* m) {
    bromesh::spatialSortVertices(M(m));
}

void generateShadowIndexBuffer(void* m, bronze_native_buffer* out) {
    transferOut(bromesh::generateShadowIndexBuffer(M(m)), out);
}

void stripify(const uint32_t* indices, uint32_t in, int32_t vertexCount, bronze_native_buffer* out) {
    std::vector<uint32_t> idx(indices, indices + in);
    transferOut(bromesh::stripify(idx, static_cast<size_t>(vertexCount > 0 ? vertexCount : 0)), out);
}

void unstripify(const uint32_t* strip, uint32_t sn, bronze_native_buffer* out) {
    std::vector<uint32_t> st(strip, strip + sn);
    transferOut(bromesh::unstripify(st), out);
}

void encodeMesh(void* m) {
    g_encodedMesh = bromesh::encodeMesh(M(m));
}

void encodedVertexData(bronze_native_buffer* out) { copyOut(g_encodedMesh.vertexData, out); }
void encodedIndexData(bronze_native_buffer* out) { copyOut(g_encodedMesh.indexData, out); }
double encodedVertexCount() { return static_cast<double>(g_encodedMesh.vertexCount); }
double encodedVertexSize() { return static_cast<double>(g_encodedMesh.vertexSize); }
double encodedIndexCount() { return static_cast<double>(g_encodedMesh.indexCount); }

void* decodeMesh(const uint8_t* vdata, uint32_t vn, const uint8_t* idata, uint32_t in,
                int32_t vertexCount, int32_t vertexSize, int32_t indexCount,
                bool hasNormals, bool hasUVs, bool hasColors) {
    bromesh::EncodedMesh enc;
    enc.vertexData.assign(vdata, vdata + vn);
    enc.indexData.assign(idata, idata + in);
    enc.vertexCount = static_cast<size_t>(vertexCount > 0 ? vertexCount : 0);
    enc.vertexSize = static_cast<size_t>(vertexSize > 0 ? vertexSize : 12);
    enc.indexCount = static_cast<size_t>(indexCount > 0 ? indexCount : 0);
    return take(bromesh::decodeMesh(enc, hasNormals, hasUVs, hasColors));
}

// ---- ProgressiveMesh -------------------------------------------------------

void* pmNew(void* mesh) {
    return new bromesh::ProgressiveMesh(bromesh::buildProgressiveMesh(M(mesh)));
}

void pmFree(void* p) {
    delete static_cast<bromesh::ProgressiveMesh*>(p);
}

double pmMaxTriangles(void* pm) {
    return static_cast<double>(static_cast<bromesh::ProgressiveMesh*>(pm)->maxTriangles());
}

double pmMinTriangles(void* pm) {
    return static_cast<double>(static_cast<bromesh::ProgressiveMesh*>(pm)->minTriangles());
}

void* pmAtRatio(void* pm, double ratio) {
    auto& p = *static_cast<bromesh::ProgressiveMesh*>(pm);
    return take(bromesh::progressiveMeshAtRatio(p, static_cast<float>(ratio)));
}

void* pmAtTriangleCount(void* pm, int32_t count) {
    auto& p = *static_cast<bromesh::ProgressiveMesh*>(pm);
    return take(bromesh::progressiveMeshAtTriangleCount(p, static_cast<size_t>(count > 0 ? count : 0)));
}

void pmSerialize(void* pm, bronze_native_buffer* out) {
    auto& p = *static_cast<bromesh::ProgressiveMesh*>(pm);
    transferOut(bromesh::serializeProgressiveMesh(p), out);
}

void* pmDeserialize(const uint8_t* data, uint32_t n) {
    return new bromesh::ProgressiveMesh(bromesh::deserializeProgressiveMesh(data, static_cast<size_t>(n)));
}

// ---- Isosurfaces & Voxels --------------------------------------------------

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

void* transvoxel(const float* field, uint32_t fn, int32_t gridSize, int32_t lod,
                 const int32_t* neighborLods, uint32_t nn, double isoLevel, double cellSize) {
    if (gridSize <= 0 || static_cast<uint64_t>(fn) != static_cast<uint64_t>(gridSize) * gridSize * gridSize) {
        ev::throwTypeError("Mesh.transvoxel: field length must be gridSize^3");
        return nullptr;
    }
    int nl[6] = {-1, -1, -1, -1, -1, -1};
    if (neighborLods && nn >= 6) {
        for (int i = 0; i < 6; ++i) nl[i] = neighborLods[i];
    }
    return take(bromesh::transvoxel(field, gridSize, lod, nl, static_cast<float>(isoLevel),
                                    static_cast<float>(cellSize > 0.0 ? cellSize : 1.0f)));
}

void* greedyMesh(const uint8_t* voxels, uint32_t vn, int32_t gx, int32_t gy, int32_t gz,
                 double cellSize) {
    if (gx <= 0 || gy <= 0 || gz <= 0 ||
        static_cast<uint64_t>(vn) != static_cast<uint64_t>(gx) * gy * gz) {
        ev::throwTypeError("Mesh.greedyMesh: voxels length must be gx*gy*gz");
        return nullptr;
    }
    return take(bromesh::greedyMesh(voxels, gx, gy, gz, static_cast<float>(cellSize > 0.0 ? cellSize : 1.0f)));
}

// ---- Draco -----------------------------------------------------------------

#if BROMESH_HAS_DRACO
const char* dracoDecode(const uint8_t* bytes, uint32_t n) {
    g_draco = bromesh::decodeDraco(bytes, n);
    return natives::strResult(g_draco.error);
}

void* dracoTakeMesh() {
    return take(std::move(g_draco.mesh));
}

int32_t dracoAttributeCount() {
    return static_cast<int32_t>(g_draco.attributes.size());
}

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

void dracoAttributeBytes(int32_t i, bronze_native_buffer* out) {
    if (i < 0 || static_cast<size_t>(i) >= g_draco.attributes.size()) {
        ev::throwRangeError("Draco attribute " + std::to_string(i) + " is not in the snapshot");
        return;
    }
    copyOut(g_draco.attributes[static_cast<size_t>(i)].bytes, out);
}

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

}  // namespace bro::bronze_host

#endif // BRO_WITH_3D
