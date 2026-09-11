// =============================================================================
// bro_mesh_c_abi.h — Pure C-ABI declarations for bro.mesh
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_MESH_C_ABI_H
#define BRO_MESH_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.mesh.Mesh ---
void* bro_Mesh_create(void* opts);
void  bro_Mesh_destroy(void* self);
void* bro_Mesh_get_positions(void* self);
void bro_Mesh_set_positions(void* self, void* val);
void* bro_Mesh_get_normals(void* self);
void bro_Mesh_set_normals(void* self, void* val);
void* bro_Mesh_get_uvs(void* self);
void bro_Mesh_set_uvs(void* self, void* val);
void* bro_Mesh_get_colors(void* self);
void bro_Mesh_set_colors(void* self, void* val);
void* bro_Mesh_get_indices(void* self);
void bro_Mesh_set_indices(void* self, void* val);
int32_t bro_Mesh_get_vertexCount(void* self);
int32_t bro_Mesh_get_triangleCount(void* self);
bool bro_Mesh_get_hasNormals(void* self);
bool bro_Mesh_get_hasUVs(void* self);
bool bro_Mesh_get_hasColors(void* self);
bool bro_Mesh_get_empty(void* self);
void* bro_Mesh_clone(void* self);
void* bro_Mesh_translate(void* self, double dx, double dy, double dz);
void* bro_Mesh_scale(void* self, double sx, double sy, double sz);
void* bro_Mesh_rotate(void* self, double ax, double ay, double az, double angle);
void* bro_Mesh_center(void* self);
void* bro_Mesh_fitToBox(void* self, double size);
void* bro_Mesh_transform(void* self, void* matrix);
void* bro_Mesh_applySkinning(void* self, void* skin, void* matrices);
void* bro_Mesh_applyMorphTarget(void* self, void* target, double weight);
void* bro_Mesh_computeNormals(void* self, double creaseAngle);
void* bro_Mesh_invertNormals(void* self);
void* bro_Mesh_flipFaces(void* self);
void* bro_Mesh_weld(void* self, double threshold);
void* bro_Mesh_simplify(void* self, double ratio, double targetError);
void* bro_Mesh_subdivideLoop(void* self, int32_t iterations);
void* bro_Mesh_subdivideCatmullClark(void* self, int32_t iterations);
void* bro_Mesh_smooth(void* self, double lambda, int32_t iterations);
void* bro_Mesh_remesh(void* self, double targetEdgeLength);
void* bro_Mesh_repair(void* self);
void* bro_Mesh_repairSelfIntersections(void* self);
void* bro_Mesh_shrinkwrap(void* self, void* target, double factor, double offset);
void* bro_Mesh_splitComponents(void* self);
void* bro_Mesh_booleanUnion(void* self, void* other);
void* bro_Mesh_booleanDifference(void* self, void* other);
void* bro_Mesh_booleanIntersection(void* self, void* other);
void* bro_Mesh_csgUnion(void* self, void* other);
void* bro_Mesh_csgSubtract(void* self, void* other);
void* bro_Mesh_csgIntersect(void* self, void* other);
void* bro_Mesh_generateUVs(void* self, const char* method);
void* bro_Mesh_projectUVs(void* self, const char* projection, void* plane);
void* bro_Mesh_optimize(void* self);
void* bro_Mesh_buildBVH(void* self);
void* bro_Mesh_buildProgressiveMesh(void* self);
void* bro_Mesh_buildMeshlets(void* self, int32_t maxVertices, int32_t maxTriangles);
void* bro_Mesh_computeUVDistortion(void* self);
void* bro_Mesh_measureUVQuality(void* self);
void* bro_Mesh_convexHull(void* self);
void* bro_Mesh_convexDecomposition(void* self, void* params);
void* bro_Mesh_toGLTF(void* self, const char* name);
const char* bro_Mesh_toOBJ(void* self);
const char* bro_Mesh_toPLY(void* self);
const char* bro_Mesh_toSTL(void* self);
void* bro_Mesh_toSTLB(void* self);
void* bro_Mesh_toFBX(void* self);
void* bro_Mesh_toVOX(void* self);
void* bro_Mesh_box(double halfW, double halfH, double halfD);
void* bro_Mesh_sphere(double radius, int32_t segments, int32_t rings);
void* bro_Mesh_cylinder(double radius, double halfHeight, int32_t segments);
void* bro_Mesh_capsule(double radius, double halfHeight, int32_t segments);
void* bro_Mesh_cone(double radius, double height, int32_t segments);
void* bro_Mesh_plane(double halfW, double halfH, int32_t segW, int32_t segH);
void* bro_Mesh_torus(double radius, double tubeRadius, int32_t segments, int32_t tubeSegments);
void* bro_Mesh_icosahedron(double radius);
void* bro_Mesh_dodecahedron(double radius);
void* bro_Mesh_octahedron(double radius);
void* bro_Mesh_tetrahedron(double radius);
void* bro_Mesh_disk(double radius, int32_t segments);
void* bro_Mesh_tube(void* points, double radius, int32_t segments);
void* bro_Mesh_fromGLTF(void* buffer);
void* bro_Mesh_fromOBJ(const char* text);
void* bro_Mesh_fromPLY(void* buffer);
void* bro_Mesh_fromSTL(void* buffer);
void* bro_Mesh_fromFBX(void* buffer);
void* bro_Mesh_fromVOX(void* buffer);
void* bro_Mesh_merge(void* meshes);
void* bro_Mesh_marchingCubes(void* values, int32_t dimX, int32_t dimY, int32_t dimZ, double isoLevel);
void* bro_Mesh_surfaceNets(void* values, int32_t dimX, int32_t dimY, int32_t dimZ, double isoLevel);
void* bro_Mesh_dualContouring(void* values, int32_t dimX, int32_t dimY, int32_t dimZ, double isoLevel);
void* bro_Mesh_sweep(void* path, void* profile);
void* bro_Mesh_bezierSweep(void* controlPoints, void* profile, void* opts);
void* bro_Mesh_leafCard(void* shape, void* opts);
void* bro_Mesh_flower(void* opts);
void* bro_Mesh_blob(void* opts);
void* bro_Mesh_spaceColonize(void* attractors, void* seedPoints, void* initialDirection, void* opts);
void* bro_Mesh_thickenBranches(void* segments, double leafRadius, double pipeExp);
void* bro_Mesh_meshBranches(void* segments, int32_t sides);
void* bro_Mesh_placeLeavesOnBranches(void* segments, void* opts);
void* bro_Mesh_scatterLeaves(void* segments, void* leaf, void* opts);
void* bro_Mesh_tree(void* opts);
void* bro_Mesh_parseLSystem(const char* text);
void* bro_Mesh_packAnchors(void* candidates, void* opts);
void* bro_Mesh_lsystemToBranches(void* modules, void* opts);
void* bro_Mesh_capsuleField(void* capsules, void* spheres, double cellSize);
void* bro_Mesh_capsuleFieldFromSegments(void* segments, double radiusScale, void* extraSpheres);
void* bro_Mesh_decodeDraco(void* bytes);
void* bro_Mesh_encodeDraco(void* meshData, void* opts);

// --- Interface bro.mesh.MeshBVH ---
void* bro_MeshBVH_create(void);
void  bro_MeshBVH_destroy(void* self);
void* bro_MeshBVH_raycast(void* self, void* origin, void* direction, double maxDist);
void* bro_MeshBVH_queryAABB(void* self, void* min, void* max);

// --- Interface bro.mesh.ProgressiveMesh ---
void* bro_ProgressiveMesh_create(void);
void  bro_ProgressiveMesh_destroy(void* self);
void* bro_ProgressiveMesh_getMesh(void* self, double detail);
int32_t bro_ProgressiveMesh_get_collapseCount(void* self);
int32_t bro_ProgressiveMesh_get_minVertices(void* self);
int32_t bro_ProgressiveMesh_get_maxVertices(void* self);

// --- Interface bro.mesh.PolyMesh ---
void* bro_PolyMesh_create(void* mesh);
void  bro_PolyMesh_destroy(void* self);
int32_t bro_PolyMesh_get_vertexCount(void* self);
int32_t bro_PolyMesh_get_faceCount(void* self);
int32_t bro_PolyMesh_get_edgeCount(void* self);
void* bro_PolyMesh_toMesh(void* self);
void* bro_PolyMesh_extrudeFace(void* self, int32_t faceIdx, void* offset, bool withBack, int32_t bridgeGroup, int32_t backGroup);
void bro_PolyMesh_rematchTwins(void* self);
void bro_PolyMesh_mergeFacesByGroup(void* self);
void bro_PolyMesh_compact(void* self);
void* bro_PolyMesh_findGroupBoundary(void* self, int32_t groupId);

// --- Interface bro.mesh.LSystem ---
void* bro_LSystem_create(const char* axiom);
void  bro_LSystem_destroy(void* self);
void* bro_LSystem_setAxiom(void* self, const char* text);
void* bro_LSystem_addRule(void* self, const char* predecessor, const char* successor, double weight);
const char* bro_LSystem_derive(void* self, int32_t iterations, int64_t seed);
void* bro_LSystem_deriveModules(void* self, int32_t iterations, int64_t seed);

#ifdef __cplusplus
}
#endif

#endif // BRO_MESH_C_ABI_H
