// =============================================================================
// bro_mesh_c_abi.cpp — C++ forwarding implementations for bro.mesh
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_mesh_c_abi.h"
#include <cstdint>
#include <string>

struct BroMeshImpl {
    int32_t vertexCount = 0;
    int32_t triangleCount = 0;
    bool hasNormals = false;
    bool hasUVs = false;
    bool hasColors = false;
    bool empty = true;
};

struct BroMeshBVHImpl {
};

struct BroProgressiveMeshImpl {
    int32_t collapseCount = 0;
    int32_t minVertices = 0;
    int32_t maxVertices = 0;
};

struct BroPolyMeshImpl {
    int32_t vertexCount = 0;
    int32_t faceCount = 0;
    int32_t edgeCount = 0;
};

struct BroLSystemImpl {
    std::string axiom;
    std::string result;
};

extern "C" {

// --- Interface bro.mesh.Mesh ---
void* bro_Mesh_create(void* /*opts*/) {
    return new BroMeshImpl();
}

void bro_Mesh_destroy(void* self) {
    delete static_cast<BroMeshImpl*>(self);
}

void* bro_Mesh_get_positions(void* /*self*/) { return nullptr; }
void bro_Mesh_set_positions(void* /*self*/, void* /*val*/) {}
void* bro_Mesh_get_normals(void* /*self*/) { return nullptr; }
void bro_Mesh_set_normals(void* /*self*/, void* /*val*/) {}
void* bro_Mesh_get_uvs(void* /*self*/) { return nullptr; }
void bro_Mesh_set_uvs(void* /*self*/, void* /*val*/) {}
void* bro_Mesh_get_colors(void* /*self*/) { return nullptr; }
void bro_Mesh_set_colors(void* /*self*/, void* /*val*/) {}
void* bro_Mesh_get_indices(void* /*self*/) { return nullptr; }
void bro_Mesh_set_indices(void* /*self*/, void* /*val*/) {}

int32_t bro_Mesh_get_vertexCount(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->vertexCount : 0;
}

int32_t bro_Mesh_get_triangleCount(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->triangleCount : 0;
}

bool bro_Mesh_get_hasNormals(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->hasNormals : false;
}

bool bro_Mesh_get_hasUVs(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->hasUVs : false;
}

bool bro_Mesh_get_hasColors(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->hasColors : false;
}

bool bro_Mesh_get_empty(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    return m ? m->empty : true;
}

void* bro_Mesh_clone(void* self) {
    auto* m = static_cast<BroMeshImpl*>(self);
    auto* res = new BroMeshImpl();
    if (m) *res = *m;
    return res;
}

void* bro_Mesh_translate(void* self, double /*dx*/, double /*dy*/, double /*dz*/) { return self; }
void* bro_Mesh_scale(void* self, double /*sx*/, double /*sy*/, double /*sz*/) { return self; }
void* bro_Mesh_rotate(void* self, double /*ax*/, double /*ay*/, double /*az*/, double /*angle*/) { return self; }
void* bro_Mesh_center(void* self) { return self; }
void* bro_Mesh_fitToBox(void* self, double /*size*/) { return self; }
void* bro_Mesh_transform(void* self, void* /*matrix*/) { return self; }
void* bro_Mesh_applySkinning(void* self, void* /*skin*/, void* /*matrices*/) { return self; }
void* bro_Mesh_applyMorphTarget(void* self, void* /*target*/, double /*weight*/) { return self; }
void* bro_Mesh_computeNormals(void* self, double /*creaseAngle*/) { return self; }
void* bro_Mesh_invertNormals(void* self) { return self; }
void* bro_Mesh_flipFaces(void* self) { return self; }
void* bro_Mesh_weld(void* self, double /*threshold*/) { return self; }
void* bro_Mesh_simplify(void* self, double /*ratio*/, double /*targetError*/) { return self; }
void* bro_Mesh_subdivideLoop(void* self, int32_t /*iterations*/) { return self; }
void* bro_Mesh_subdivideCatmullClark(void* self, int32_t /*iterations*/) { return self; }
void* bro_Mesh_smooth(void* self, double /*lambda*/, int32_t /*iterations*/) { return self; }
void* bro_Mesh_remesh(void* self, double /*targetEdgeLength*/) { return self; }
void* bro_Mesh_repair(void* self) { return self; }
void* bro_Mesh_repairSelfIntersections(void* self) { return self; }
void* bro_Mesh_shrinkwrap(void* self, void* /*target*/, double /*factor*/, double /*offset*/) { return self; }
void* bro_Mesh_splitComponents(void* self) { return self; }
void* bro_Mesh_booleanUnion(void* self, void* /*other*/) { return self; }
void* bro_Mesh_booleanDifference(void* self, void* /*other*/) { return self; }
void* bro_Mesh_booleanIntersection(void* self, void* /*other*/) { return self; }
void* bro_Mesh_csgUnion(void* self, void* /*other*/) { return self; }
void* bro_Mesh_csgSubtract(void* self, void* /*other*/) { return self; }
void* bro_Mesh_csgIntersect(void* self, void* /*other*/) { return self; }
void* bro_Mesh_generateUVs(void* self, const char* /*method*/) { return self; }
void* bro_Mesh_projectUVs(void* self, const char* /*projection*/, void* /*plane*/) { return self; }
void* bro_Mesh_optimize(void* self) { return self; }
void* bro_Mesh_buildBVH(void* /*self*/) { return new BroMeshBVHImpl(); }
void* bro_Mesh_buildProgressiveMesh(void* /*self*/) { return new BroProgressiveMeshImpl(); }
void* bro_Mesh_buildMeshlets(void* /*self*/, int32_t /*maxVertices*/, int32_t /*maxTriangles*/) { return nullptr; }
void* bro_Mesh_computeUVDistortion(void* /*self*/) { return nullptr; }
void* bro_Mesh_measureUVQuality(void* /*self*/) { return nullptr; }
void* bro_Mesh_convexHull(void* self) { return self; }
void* bro_Mesh_convexDecomposition(void* /*self*/, void* /*params*/) { return nullptr; }
void* bro_Mesh_toGLTF(void* /*self*/, const char* /*name*/) { return nullptr; }
const char* bro_Mesh_toOBJ(void* /*self*/) { return ""; }
const char* bro_Mesh_toPLY(void* /*self*/) { return ""; }
const char* bro_Mesh_toSTL(void* /*self*/) { return ""; }
void* bro_Mesh_toSTLB(void* /*self*/) { return nullptr; }
void* bro_Mesh_toFBX(void* /*self*/) { return nullptr; }
void* bro_Mesh_toVOX(void* /*self*/) { return nullptr; }

void* bro_Mesh_box(double /*halfW*/, double /*halfH*/, double /*halfD*/) { return new BroMeshImpl(); }
void* bro_Mesh_sphere(double /*radius*/, int32_t /*segments*/, int32_t /*rings*/) { return new BroMeshImpl(); }
void* bro_Mesh_cylinder(double /*radius*/, double /*halfHeight*/, int32_t /*segments*/) { return new BroMeshImpl(); }
void* bro_Mesh_capsule(double /*radius*/, double /*halfHeight*/, int32_t /*segments*/) { return new BroMeshImpl(); }
void* bro_Mesh_cone(double /*radius*/, double /*height*/, int32_t /*segments*/) { return new BroMeshImpl(); }
void* bro_Mesh_plane(double /*halfW*/, double /*halfH*/, int32_t /*segW*/, int32_t /*segH*/) { return new BroMeshImpl(); }
void* bro_Mesh_torus(double /*radius*/, double /*tubeRadius*/, int32_t /*segments*/, int32_t /*tubeSegments*/) { return new BroMeshImpl(); }
void* bro_Mesh_icosahedron(double /*radius*/) { return new BroMeshImpl(); }
void* bro_Mesh_dodecahedron(double /*radius*/) { return new BroMeshImpl(); }
void* bro_Mesh_octahedron(double /*radius*/) { return new BroMeshImpl(); }
void* bro_Mesh_tetrahedron(double /*radius*/) { return new BroMeshImpl(); }
void* bro_Mesh_disk(double /*radius*/, int32_t /*segments*/) { return new BroMeshImpl(); }
void* bro_Mesh_tube(void* /*points*/, double /*radius*/, int32_t /*segments*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromGLTF(void* /*buffer*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromOBJ(const char* /*text*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromPLY(void* /*buffer*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromSTL(void* /*buffer*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromFBX(void* /*buffer*/) { return new BroMeshImpl(); }
void* bro_Mesh_fromVOX(void* /*buffer*/) { return new BroMeshImpl(); }
void* bro_Mesh_merge(void* /*meshes*/) { return new BroMeshImpl(); }
void* bro_Mesh_marchingCubes(void* /*values*/, int32_t /*dimX*/, int32_t /*dimY*/, int32_t /*dimZ*/, double /*isoLevel*/) { return new BroMeshImpl(); }
void* bro_Mesh_surfaceNets(void* /*values*/, int32_t /*dimX*/, int32_t /*dimY*/, int32_t /*dimZ*/, double /*isoLevel*/) { return new BroMeshImpl(); }
void* bro_Mesh_dualContouring(void* /*values*/, int32_t /*dimX*/, int32_t /*dimY*/, int32_t /*dimZ*/, double /*isoLevel*/) { return new BroMeshImpl(); }
void* bro_Mesh_sweep(void* /*path*/, void* /*profile*/) { return new BroMeshImpl(); }
void* bro_Mesh_bezierSweep(void* /*controlPoints*/, void* /*profile*/, void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_leafCard(void* /*shape*/, void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_flower(void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_blob(void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_spaceColonize(void* /*attractors*/, void* /*seedPoints*/, void* /*initialDirection*/, void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_thickenBranches(void* /*segments*/, double /*leafRadius*/, double /*pipeExp*/) { return new BroMeshImpl(); }
void* bro_Mesh_meshBranches(void* /*segments*/, int32_t /*sides*/) { return new BroMeshImpl(); }
void* bro_Mesh_placeLeavesOnBranches(void* /*segments*/, void* /*opts*/) { return nullptr; }
void* bro_Mesh_scatterLeaves(void* /*segments*/, void* /*leaf*/, void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_tree(void* /*opts*/) { return new BroMeshImpl(); }
void* bro_Mesh_parseLSystem(const char* /*text*/) { return nullptr; }
void* bro_Mesh_packAnchors(void* /*candidates*/, void* /*opts*/) { return nullptr; }
void* bro_Mesh_lsystemToBranches(void* /*modules*/, void* /*opts*/) { return nullptr; }
void* bro_Mesh_capsuleField(void* /*capsules*/, void* /*spheres*/, double /*cellSize*/) { return nullptr; }
void* bro_Mesh_capsuleFieldFromSegments(void* /*segments*/, double /*radiusScale*/, void* /*extraSpheres*/) { return nullptr; }
void* bro_Mesh_decodeDraco(void* /*bytes*/) { return nullptr; }
void* bro_Mesh_encodeDraco(void* /*meshData*/, void* /*opts*/) { return nullptr; }

// --- Interface bro.mesh.MeshBVH ---
void* bro_MeshBVH_create(void) {
    return new BroMeshBVHImpl();
}

void bro_MeshBVH_destroy(void* self) {
    delete static_cast<BroMeshBVHImpl*>(self);
}

void* bro_MeshBVH_raycast(void* /*self*/, void* /*origin*/, void* /*direction*/, double /*maxDist*/) { return nullptr; }
void* bro_MeshBVH_queryAABB(void* /*self*/, void* /*min*/, void* /*max*/) { return nullptr; }

// --- Interface bro.mesh.ProgressiveMesh ---
void* bro_ProgressiveMesh_create(void) {
    return new BroProgressiveMeshImpl();
}

void bro_ProgressiveMesh_destroy(void* self) {
    delete static_cast<BroProgressiveMeshImpl*>(self);
}

void* bro_ProgressiveMesh_getMesh(void* /*self*/, double /*detail*/) { return new BroMeshImpl(); }

int32_t bro_ProgressiveMesh_get_collapseCount(void* self) {
    auto* p = static_cast<BroProgressiveMeshImpl*>(self);
    return p ? p->collapseCount : 0;
}

int32_t bro_ProgressiveMesh_get_minVertices(void* self) {
    auto* p = static_cast<BroProgressiveMeshImpl*>(self);
    return p ? p->minVertices : 0;
}

int32_t bro_ProgressiveMesh_get_maxVertices(void* self) {
    auto* p = static_cast<BroProgressiveMeshImpl*>(self);
    return p ? p->maxVertices : 0;
}

// --- Interface bro.mesh.PolyMesh ---
void* bro_PolyMesh_create(void* /*mesh*/) {
    return new BroPolyMeshImpl();
}

void bro_PolyMesh_destroy(void* self) {
    delete static_cast<BroPolyMeshImpl*>(self);
}

int32_t bro_PolyMesh_get_vertexCount(void* self) {
    auto* p = static_cast<BroPolyMeshImpl*>(self);
    return p ? p->vertexCount : 0;
}

int32_t bro_PolyMesh_get_faceCount(void* self) {
    auto* p = static_cast<BroPolyMeshImpl*>(self);
    return p ? p->faceCount : 0;
}

int32_t bro_PolyMesh_get_edgeCount(void* self) {
    auto* p = static_cast<BroPolyMeshImpl*>(self);
    return p ? p->edgeCount : 0;
}

void* bro_PolyMesh_toMesh(void* /*self*/) { return new BroMeshImpl(); }
void* bro_PolyMesh_extrudeFace(void* /*self*/, int32_t /*faceIdx*/, void* /*offset*/, bool /*withBack*/, int32_t /*bridgeGroup*/, int32_t /*backGroup*/) { return nullptr; }
void bro_PolyMesh_rematchTwins(void* /*self*/) {}
void bro_PolyMesh_mergeFacesByGroup(void* /*self*/) {}
void bro_PolyMesh_compact(void* /*self*/) {}
void* bro_PolyMesh_findGroupBoundary(void* /*self*/, int32_t /*groupId*/) { return nullptr; }

// --- Interface bro.mesh.LSystem ---
void* bro_LSystem_create(const char* axiom) {
    auto* l = new BroLSystemImpl();
    if (axiom) l->axiom = axiom;
    return l;
}

void bro_LSystem_destroy(void* self) {
    delete static_cast<BroLSystemImpl*>(self);
}

void* bro_LSystem_setAxiom(void* self, const char* text) {
    auto* l = static_cast<BroLSystemImpl*>(self);
    if (l && text) l->axiom = text;
    return self;
}

void* bro_LSystem_addRule(void* self, const char* /*predecessor*/, const char* /*successor*/, double /*weight*/) {
    return self;
}

const char* bro_LSystem_derive(void* self, int32_t /*iterations*/, int64_t /*seed*/) {
    auto* l = static_cast<BroLSystemImpl*>(self);
    return l ? l->axiom.c_str() : "";
}

void* bro_LSystem_deriveModules(void* /*self*/, int32_t /*iterations*/, int64_t /*seed*/) {
    return nullptr;
}

} // extern "C"