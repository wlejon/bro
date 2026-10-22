// native_scene_decl.h — the C entry points native_scene_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper and in its body together. The bodies are
// hand-written in bro against the engine, and the compiler checks them here.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   double / int32_t / bool      f64 / i32 / bool
//   const char*                  str — UTF-8, valid for the call only; a RESULT must
//                                outlive the return (a per-thread scratch), and one
//                                marked JSON carries JSON of the type named in the comment
//   uint64_t                     dynamic — a callback's raw Value bits; keep it in a
//                                Persistent, never across an allocating call raw
//   (const T*, uint32_t)         a typed array: elements and count, valid for the call
//   void*                        a class handle: what the class constructor returned
//   bronze_native_buffer* out    a typed-array RESULT: the function returns void and
//                                fills *out (data, length in elements); release == NULL
//                                and the runtime copies during the call; release set
//                                ([transfer]) and the array is a view over data, with
//                                release(ctx) run when it is collected
//   bool <x>_given, T <x>        an optional value with no declared default, and
//                                whether it was passed
//
// A dictionary or list RESULT crosses as reads: the operation runs and keeps its
// result in a per-thread slot, answering nothing (or the list's length), and the
// <op>_<member> / <op>_at natives read from that slot until the next call.
//
// Names classes of: animation, terrain — register those subsystems first.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// destructor of __bro_native.scene.SceneNode: frees what the constructor (or a native returning a SceneNode) allocated
void bro_scene_SceneNode_dtor(void* self);

// constructor of __bro_native.scene.SceneNode: registered so the class exists (instances come from natives returning a SceneNode); never called by the wrapper, the body may return NULL
//   registered at __bro_native.scene.SceneNode
void* bro_scene_SceneNode_ctor(void);

// destructor of __bro_native.scene.SceneGraph: frees what the constructor (or a native returning a SceneGraph) allocated
void bro_scene_SceneGraph_dtor(void* self);

// constructor of __bro_native.scene.SceneGraph: registered so the class exists (instances come from natives returning a SceneGraph); never called by the wrapper, the body may return NULL
//   registered at __bro_native.scene.SceneGraph
void* bro_scene_SceneGraph_ctor(void);

// bro.scene.SceneNode.prototype.id read
//   registered at __bro_native.scene.SceneNode_id_get
int32_t bro_scene_SceneNode_id_get(void* self);

// bro.scene.SceneNode.prototype.name read
//   registered at __bro_native.scene.SceneNode_name_get
const char* bro_scene_SceneNode_name_get(void* self);

// bro.scene.SceneNode.prototype.name write
//   registered at __bro_native.scene.SceneNode_name_set
void bro_scene_SceneNode_name_set(void* self, const char* v);

// bro.scene.SceneNode.prototype.visible read
//   registered at __bro_native.scene.SceneNode_visible_get
bool bro_scene_SceneNode_visible_get(void* self);

// bro.scene.SceneNode.prototype.visible write
//   registered at __bro_native.scene.SceneNode_visible_set
void bro_scene_SceneNode_visible_set(void* self, bool v);

// bro.scene.SceneNode.prototype.x read
//   registered at __bro_native.scene.SceneNode_x_get
double bro_scene_SceneNode_x_get(void* self);

// bro.scene.SceneNode.prototype.x write
//   registered at __bro_native.scene.SceneNode_x_set
void bro_scene_SceneNode_x_set(void* self, double v);

// bro.scene.SceneNode.prototype.y read
//   registered at __bro_native.scene.SceneNode_y_get
double bro_scene_SceneNode_y_get(void* self);

// bro.scene.SceneNode.prototype.y write
//   registered at __bro_native.scene.SceneNode_y_set
void bro_scene_SceneNode_y_set(void* self, double v);

// bro.scene.SceneNode.prototype.z read
//   registered at __bro_native.scene.SceneNode_z_get
double bro_scene_SceneNode_z_get(void* self);

// bro.scene.SceneNode.prototype.z write
//   registered at __bro_native.scene.SceneNode_z_set
void bro_scene_SceneNode_z_set(void* self, double v);

// bro.scene.SceneNode.prototype.position read
//   registered at __bro_native.scene.SceneNode_position_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_position_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.position write
//   registered at __bro_native.scene.SceneNode_position_set
void bro_scene_SceneNode_position_set(void* self, const double* v, uint32_t v_len);

// bro.scene.SceneNode.prototype.rotation read
//   registered at __bro_native.scene.SceneNode_rotation_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_rotation_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.rotation write
//   registered at __bro_native.scene.SceneNode_rotation_set
void bro_scene_SceneNode_rotation_set(void* self, const double* v, uint32_t v_len);

// bro.scene.SceneNode.prototype.rotationX read
//   registered at __bro_native.scene.SceneNode_rotationX_get
double bro_scene_SceneNode_rotationX_get(void* self);

// bro.scene.SceneNode.prototype.rotationX write
//   registered at __bro_native.scene.SceneNode_rotationX_set
void bro_scene_SceneNode_rotationX_set(void* self, double v);

// bro.scene.SceneNode.prototype.rotationY read
//   registered at __bro_native.scene.SceneNode_rotationY_get
double bro_scene_SceneNode_rotationY_get(void* self);

// bro.scene.SceneNode.prototype.rotationY write
//   registered at __bro_native.scene.SceneNode_rotationY_set
void bro_scene_SceneNode_rotationY_set(void* self, double v);

// bro.scene.SceneNode.prototype.rotationZ read
//   registered at __bro_native.scene.SceneNode_rotationZ_get
double bro_scene_SceneNode_rotationZ_get(void* self);

// bro.scene.SceneNode.prototype.rotationZ write
//   registered at __bro_native.scene.SceneNode_rotationZ_set
void bro_scene_SceneNode_rotationZ_set(void* self, double v);

// bro.scene.SceneNode.prototype.scale read
//   registered at __bro_native.scene.SceneNode_scale_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_scale_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.scale write
//   registered at __bro_native.scene.SceneNode_scale_set
void bro_scene_SceneNode_scale_set(void* self, const double* v, uint32_t v_len);

// bro.scene.SceneNode.prototype.scaleX read
//   registered at __bro_native.scene.SceneNode_scaleX_get
double bro_scene_SceneNode_scaleX_get(void* self);

// bro.scene.SceneNode.prototype.scaleX write
//   registered at __bro_native.scene.SceneNode_scaleX_set
void bro_scene_SceneNode_scaleX_set(void* self, double v);

// bro.scene.SceneNode.prototype.scaleY read
//   registered at __bro_native.scene.SceneNode_scaleY_get
double bro_scene_SceneNode_scaleY_get(void* self);

// bro.scene.SceneNode.prototype.scaleY write
//   registered at __bro_native.scene.SceneNode_scaleY_set
void bro_scene_SceneNode_scaleY_set(void* self, double v);

// bro.scene.SceneNode.prototype.scaleZ read
//   registered at __bro_native.scene.SceneNode_scaleZ_get
double bro_scene_SceneNode_scaleZ_get(void* self);

// bro.scene.SceneNode.prototype.scaleZ write
//   registered at __bro_native.scene.SceneNode_scaleZ_set
void bro_scene_SceneNode_scaleZ_set(void* self, double v);

// bro.scene.SceneNode.prototype.quaternion read
//   registered at __bro_native.scene.SceneNode_quaternion_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_quaternion_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.quaternion write
//   registered at __bro_native.scene.SceneNode_quaternion_set
void bro_scene_SceneNode_quaternion_set(void* self, const double* v, uint32_t v_len);

// bro.scene.SceneNode.prototype.fov read
//   registered at __bro_native.scene.SceneNode_fov_get
double bro_scene_SceneNode_fov_get(void* self);

// bro.scene.SceneNode.prototype.fov write
//   registered at __bro_native.scene.SceneNode_fov_set
void bro_scene_SceneNode_fov_set(void* self, double v);

// bro.scene.SceneNode.prototype.near read
//   registered at __bro_native.scene.SceneNode_near_get
double bro_scene_SceneNode_near_get(void* self);

// bro.scene.SceneNode.prototype.near write
//   registered at __bro_native.scene.SceneNode_near_set
void bro_scene_SceneNode_near_set(void* self, double v);

// bro.scene.SceneNode.prototype.far read
//   registered at __bro_native.scene.SceneNode_far_get
double bro_scene_SceneNode_far_get(void* self);

// bro.scene.SceneNode.prototype.far write
//   registered at __bro_native.scene.SceneNode_far_set
void bro_scene_SceneNode_far_set(void* self, double v);

// bro.scene.SceneNode.prototype.aspect read
//   registered at __bro_native.scene.SceneNode_aspect_get
double bro_scene_SceneNode_aspect_get(void* self);

// bro.scene.SceneNode.prototype.aspect write
//   registered at __bro_native.scene.SceneNode_aspect_set
void bro_scene_SceneNode_aspect_set(void* self, double v);

// bro.scene.SceneNode.prototype.orthoHeight read
//   registered at __bro_native.scene.SceneNode_orthoHeight_get
double bro_scene_SceneNode_orthoHeight_get(void* self);

// bro.scene.SceneNode.prototype.orthoHeight write
//   registered at __bro_native.scene.SceneNode_orthoHeight_set
void bro_scene_SceneNode_orthoHeight_set(void* self, double v);

// bro.scene.SceneNode.prototype.projection read
//   registered at __bro_native.scene.SceneNode_projection_get
const char* bro_scene_SceneNode_projection_get(void* self);

// bro.scene.SceneNode.prototype.projection write
//   registered at __bro_native.scene.SceneNode_projection_set
void bro_scene_SceneNode_projection_set(void* self, const char* v);

// bro.scene.SceneNode.prototype.worldPosition read
//   registered at __bro_native.scene.SceneNode_worldPosition_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_worldPosition_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.worldMatrix read
//   registered at __bro_native.scene.SceneNode_worldMatrix_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_worldMatrix_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneNode.prototype.parent read
//   registered at __bro_native.scene.SceneNode_parent_get
void* bro_scene_SceneNode_parent_get(void* self);

// bro.scene.SceneNode.prototype.children: runs the operation, keeps the list in a per-thread slot, and answers its length; SceneNode_children_get_at(index) reads from it
//   registered at __bro_native.scene.SceneNode_children_get
int32_t bro_scene_SceneNode_children_get(void* self);

// bro.scene.SceneNode.prototype.children: element [index] of the list SceneNode_children_get kept, as a SceneNode handle
//   registered at __bro_native.scene.SceneNode_children_get_at
void* bro_scene_SceneNode_children_get_at(int32_t index);

// bro.scene.SceneNode.prototype.add
//   registered at __bro_native.scene.SceneNode_add
void* bro_scene_SceneNode_add(void* self, void* child);

// bro.scene.SceneNode.prototype.remove
//   registered at __bro_native.scene.SceneNode_remove
void bro_scene_SceneNode_remove(void* self, void* child);

// bro.scene.SceneNode.prototype.addChild
//   registered at __bro_native.scene.SceneNode_addChild
void* bro_scene_SceneNode_addChild(void* self, void* child);

// bro.scene.SceneNode.prototype.removeChild
//   registered at __bro_native.scene.SceneNode_removeChild
void bro_scene_SceneNode_removeChild(void* self, void* child);

// bro.scene.SceneNode.prototype.destroy
//   registered at __bro_native.scene.SceneNode_destroy
void bro_scene_SceneNode_destroy(void* self);

// bro.scene.SceneNode.prototype.setPosition
//   registered at __bro_native.scene.SceneNode_setPosition
void* bro_scene_SceneNode_setPosition(void* self, double x, double y, double z);

// bro.scene.SceneNode.prototype.setRotation
//   registered at __bro_native.scene.SceneNode_setRotation
//   w_given: false when the value was not passed (no declared default)
void* bro_scene_SceneNode_setRotation(void* self, double x, double y, double z, bool w_given, double w);

// bro.scene.SceneNode.prototype.setScale
//   registered at __bro_native.scene.SceneNode_setScale
//   y_given, z_given: false when the value was not passed (no declared default)
void* bro_scene_SceneNode_setScale(void* self, double x, bool y_given, double y, bool z_given, double z);

// bro.scene.SceneNode.prototype.lookAt
//   registered at __bro_native.scene.SceneNode_lookAt
void* bro_scene_SceneNode_lookAt(void* self, const double* target, uint32_t target_len, const double* up, uint32_t up_len);

// bro.scene.SceneNode.prototype.boneCount read
//   registered at __bro_native.scene.SceneNode_boneCount_get
int32_t bro_scene_SceneNode_boneCount_get(void* self);

// bro.scene.SceneNode.prototype.skinReady read
//   registered at __bro_native.scene.SceneNode_skinReady_get
bool bro_scene_SceneNode_skinReady_get(void* self);

// bro.scene.SceneNode.prototype.isPlaying read
//   registered at __bro_native.scene.SceneNode_isPlaying_get
bool bro_scene_SceneNode_isPlaying_get(void* self);

// bro.scene.SceneNode.prototype.currentAnimation read
//   registered at __bro_native.scene.SceneNode_currentAnimation_get
const char* bro_scene_SceneNode_currentAnimation_get(void* self);

// bro.scene.SceneNode.prototype.animationDuration read
//   registered at __bro_native.scene.SceneNode_animationDuration_get
double bro_scene_SceneNode_animationDuration_get(void* self);

// bro.scene.SceneNode.prototype.animationTime read
//   registered at __bro_native.scene.SceneNode_animationTime_get
double bro_scene_SceneNode_animationTime_get(void* self);

// bro.scene.SceneNode.prototype.animationTime write
//   registered at __bro_native.scene.SceneNode_animationTime_set
void bro_scene_SceneNode_animationTime_set(void* self, double v);

// bro.scene.SceneNode.prototype.animationSpeed read
//   registered at __bro_native.scene.SceneNode_animationSpeed_get
double bro_scene_SceneNode_animationSpeed_get(void* self);

// bro.scene.SceneNode.prototype.animationSpeed write
//   registered at __bro_native.scene.SceneNode_animationSpeed_set
void bro_scene_SceneNode_animationSpeed_set(void* self, double v);

// bro.scene.SceneNode.prototype.state read
//   registered at __bro_native.scene.SceneNode_state_get
const char* bro_scene_SceneNode_state_get(void* self);

// bro.scene.SceneNode.prototype.setInstanceTransform
//   registered at __bro_native.scene.SceneNode_setInstanceTransform
void bro_scene_SceneNode_setInstanceTransform(void* self, int32_t index, const double* matrix, uint32_t matrix_len);

// bro.scene.SceneNode.prototype.setInstanceColor
//   registered at __bro_native.scene.SceneNode_setInstanceColor
void bro_scene_SceneNode_setInstanceColor(void* self, int32_t index, const double* color, uint32_t color_len);

// bro.scene.SceneNode.prototype.setInstanceCount
//   registered at __bro_native.scene.SceneNode_setInstanceCount
void bro_scene_SceneNode_setInstanceCount(void* self, int32_t count);

// bro.scene.SceneNode.prototype.setHtml
//   registered at __bro_native.scene.SceneNode_setHtml
void* bro_scene_SceneNode_setHtml(void* self, const char* html);

// bro.scene.SceneNode.prototype.markHtmlDirty
//   registered at __bro_native.scene.SceneNode_markHtmlDirty
void bro_scene_SceneNode_markHtmlDirty(void* self);

// bro.scene.SceneNode.prototype.burst
//   registered at __bro_native.scene.SceneNode_burst
void bro_scene_SceneNode_burst(void* self, int32_t count);

// bro.scene.SceneNode.prototype.clear
//   registered at __bro_native.scene.SceneNode_clear
void bro_scene_SceneNode_clear(void* self);

// bro.scene.SceneNode.prototype.probeCapture
//   registered at __bro_native.scene.SceneNode_probeCapture
void bro_scene_SceneNode_probeCapture(void* self);

// bro.scene.SceneNode.prototype.savePly
//   registered at __bro_native.scene.SceneNode_savePly
bool bro_scene_SceneNode_savePly(void* self, const char* path);

// bro.scene.SceneGraph.prototype.root read
//   registered at __bro_native.scene.SceneGraph_root_get
void* bro_scene_SceneGraph_root_get(void* self);

// bro.scene.SceneGraph.prototype.cameraX read
//   registered at __bro_native.scene.SceneGraph_cameraX_get
double bro_scene_SceneGraph_cameraX_get(void* self);

// bro.scene.SceneGraph.prototype.cameraX write
//   registered at __bro_native.scene.SceneGraph_cameraX_set
void bro_scene_SceneGraph_cameraX_set(void* self, double v);

// bro.scene.SceneGraph.prototype.cameraY read
//   registered at __bro_native.scene.SceneGraph_cameraY_get
double bro_scene_SceneGraph_cameraY_get(void* self);

// bro.scene.SceneGraph.prototype.cameraY write
//   registered at __bro_native.scene.SceneGraph_cameraY_set
void bro_scene_SceneGraph_cameraY_set(void* self, double v);

// bro.scene.SceneGraph.prototype.cameraZoom read
//   registered at __bro_native.scene.SceneGraph_cameraZoom_get
double bro_scene_SceneGraph_cameraZoom_get(void* self);

// bro.scene.SceneGraph.prototype.cameraZoom write
//   registered at __bro_native.scene.SceneGraph_cameraZoom_set
void bro_scene_SceneGraph_cameraZoom_set(void* self, double v);

// bro.scene.SceneGraph.prototype.showLightIcons read
//   registered at __bro_native.scene.SceneGraph_showLightIcons_get
bool bro_scene_SceneGraph_showLightIcons_get(void* self);

// bro.scene.SceneGraph.prototype.showLightIcons write
//   registered at __bro_native.scene.SceneGraph_showLightIcons_set
void bro_scene_SceneGraph_showLightIcons_set(void* self, bool v);

// bro.scene.SceneGraph.prototype.frustumCulling read
//   registered at __bro_native.scene.SceneGraph_frustumCulling_get
bool bro_scene_SceneGraph_frustumCulling_get(void* self);

// bro.scene.SceneGraph.prototype.frustumCulling write
//   registered at __bro_native.scene.SceneGraph_frustumCulling_set
void bro_scene_SceneGraph_frustumCulling_set(void* self, bool v);

// bro.scene.SceneGraph.prototype.renderScale read
//   registered at __bro_native.scene.SceneGraph_renderScale_get
double bro_scene_SceneGraph_renderScale_get(void* self);

// bro.scene.SceneGraph.prototype.renderScale write
//   registered at __bro_native.scene.SceneGraph_renderScale_set
void bro_scene_SceneGraph_renderScale_set(void* self, double v);

// bro.scene.SceneGraph.prototype.msaa read
//   registered at __bro_native.scene.SceneGraph_msaa_get
double bro_scene_SceneGraph_msaa_get(void* self);

// bro.scene.SceneGraph.prototype.msaa write
//   registered at __bro_native.scene.SceneGraph_msaa_set
void bro_scene_SceneGraph_msaa_set(void* self, double v);

// bro.scene.SceneGraph.prototype.activeCamera read
//   registered at __bro_native.scene.SceneGraph_activeCamera_get
void* bro_scene_SceneGraph_activeCamera_get(void* self);

// bro.scene.SceneGraph.prototype.activeCamera write
//   registered at __bro_native.scene.SceneGraph_activeCamera_set
void bro_scene_SceneGraph_activeCamera_set(void* self, void* v);

// bro.scene.SceneGraph.prototype.viewMatrix read
//   registered at __bro_native.scene.SceneGraph_viewMatrix_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_viewMatrix_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.projectionMatrix read
//   registered at __bro_native.scene.SceneGraph_projectionMatrix_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_projectionMatrix_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.cameraEye read
//   registered at __bro_native.scene.SceneGraph_cameraEye_get
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_cameraEye_get(void* self, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.createNode
//   registered at __bro_native.scene.SceneGraph_createNode
//   opts_name_given, opts_visible_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createNode(void* self, bool opts_name_given, const char* opts_name, const double* opts_position, uint32_t opts_position_len, const double* opts_rotation, uint32_t opts_rotation_len, const double* opts_scale, uint32_t opts_scale_len, bool opts_visible_given, bool opts_visible);

// bro.scene.SceneGraph.prototype.createHtmlNode
//   registered at __bro_native.scene.SceneGraph_createHtmlNode
//   opts_html_given, opts_width_given, opts_height_given, opts_visible_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createHtmlNode(void* self, bool opts_html_given, const char* opts_html, bool opts_width_given, double opts_width, bool opts_height_given, double opts_height, const double* opts_position, uint32_t opts_position_len, const double* opts_rotation, uint32_t opts_rotation_len, const double* opts_scale, uint32_t opts_scale_len, bool opts_visible_given, bool opts_visible);

// bro.scene.SceneGraph.prototype.createLight
//   registered at __bro_native.scene.SceneGraph_createLight
//   opts_type_given, opts_intensity_given, opts_range_given, opts_innerCone_given, opts_outerCone_given, opts_castShadow_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createLight(void* self, bool opts_type_given, const char* opts_type, const double* opts_color, uint32_t opts_color_len, bool opts_intensity_given, double opts_intensity, bool opts_range_given, double opts_range, bool opts_innerCone_given, double opts_innerCone, bool opts_outerCone_given, double opts_outerCone, bool opts_castShadow_given, bool opts_castShadow, const double* opts_position, uint32_t opts_position_len, const double* opts_rotation, uint32_t opts_rotation_len);

// bro.scene.SceneGraph.prototype.createParticles
//   registered at __bro_native.scene.SceneGraph_createParticles
//   jsonOpts: the ParticleNodeOptions as JSON ("" for none); body in native_scene_particles.cpp
void* bro_scene_SceneGraph_createParticles(void* self, const char* jsonOpts);

// bro.scene.SceneGraph.prototype.createDecal
//   registered at __bro_native.scene.SceneGraph_createDecal
//   opts_texture_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createDecal(void* self, bool opts_texture_given, const char* opts_texture, const double* opts_size, uint32_t opts_size_len, const double* opts_position, uint32_t opts_position_len, const double* opts_rotation, uint32_t opts_rotation_len);

// bro.scene.SceneGraph.prototype.createReflectionProbe
//   registered at __bro_native.scene.SceneGraph_createReflectionProbe
//   opts_resolution_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createReflectionProbe(void* self, const double* opts_size, uint32_t opts_size_len, bool opts_resolution_given, int32_t opts_resolution, const double* opts_position, uint32_t opts_position_len);

// bro.scene.SceneGraph.prototype.createTween
//   registered at __bro_native.scene.SceneGraph_createTween
void* bro_scene_SceneGraph_createTween(void* self);

// bro.scene.SceneGraph.prototype.createAnimationPlayer
//   registered at __bro_native.scene.SceneGraph_createAnimationPlayer
void* bro_scene_SceneGraph_createAnimationPlayer(void* self);

// bro.scene.SceneGraph.prototype.createTerrain
//   registered at __bro_native.scene.SceneGraph_createTerrain
//   opts_cellSize_given, opts_loadRadius_given, opts_unloadRadius_given, opts_maxLoadsPerUpdate_given, opts_seed_given, opts_noise_frequency_given, opts_noise_octaves_given, opts_noise_gain_given, opts_noise_lacunarity_given, opts_baseHeight_given, opts_heightAmplitude_given, opts_seaLevel_given, opts_meshMode_given, opts_terraceStep_given, opts_continentFrequency_given, opts_continentMin_given, opts_continentMax_given, opts_mountainFrequency_given, opts_mountainAmplitude_given, opts_mountainOctaves_given, opts_lodLevels_given, opts_lodScaleFactor_given, opts_planetRadius_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createTerrain(void* self, const int32_t* opts_chunkSize, uint32_t opts_chunkSize_len, bool opts_cellSize_given, double opts_cellSize, bool opts_loadRadius_given, int32_t opts_loadRadius, bool opts_unloadRadius_given, int32_t opts_unloadRadius, bool opts_maxLoadsPerUpdate_given, int32_t opts_maxLoadsPerUpdate, bool opts_seed_given, int32_t opts_seed, bool opts_noise_frequency_given, double opts_noise_frequency, bool opts_noise_octaves_given, int32_t opts_noise_octaves, bool opts_noise_gain_given, double opts_noise_gain, bool opts_noise_lacunarity_given, double opts_noise_lacunarity, bool opts_baseHeight_given, int32_t opts_baseHeight, bool opts_heightAmplitude_given, int32_t opts_heightAmplitude, bool opts_seaLevel_given, int32_t opts_seaLevel, bool opts_meshMode_given, int32_t opts_meshMode, bool opts_terraceStep_given, double opts_terraceStep, bool opts_continentFrequency_given, double opts_continentFrequency, bool opts_continentMin_given, double opts_continentMin, bool opts_continentMax_given, double opts_continentMax, bool opts_mountainFrequency_given, double opts_mountainFrequency, bool opts_mountainAmplitude_given, double opts_mountainAmplitude, bool opts_mountainOctaves_given, int32_t opts_mountainOctaves, bool opts_lodLevels_given, int32_t opts_lodLevels, bool opts_lodScaleFactor_given, int32_t opts_lodScaleFactor, bool opts_planetRadius_given, double opts_planetRadius, const double* opts_origin, uint32_t opts_origin_len, const float* opts_palette, uint32_t opts_palette_len);

// bro.scene.SceneGraph.prototype.findById
//   registered at __bro_native.scene.SceneGraph_findById
void* bro_scene_SceneGraph_findById(void* self, int32_t id);

// bro.scene.SceneGraph.prototype.findByName
//   registered at __bro_native.scene.SceneGraph_findByName
void* bro_scene_SceneGraph_findByName(void* self, const char* name);

// bro.scene.SceneGraph.prototype.destroyNode
//   registered at __bro_native.scene.SceneGraph_destroyNode
void bro_scene_SceneGraph_destroyNode(void* self, void* node);

// bro.scene.SceneGraph.prototype.setCamera
//   registered at __bro_native.scene.SceneGraph_setCamera
//   opts_fov_given, opts_near_given, opts_far_given, opts_aspect_given, opts_mode_given, opts_size_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setCamera(void* self, bool opts_fov_given, double opts_fov, bool opts_near_given, double opts_near, bool opts_far_given, double opts_far, const double* opts_eye, uint32_t opts_eye_len, const double* opts_target, uint32_t opts_target_len, const double* opts_up, uint32_t opts_up_len, bool opts_aspect_given, double opts_aspect, const double* opts_quaternion, uint32_t opts_quaternion_len, bool opts_mode_given, const char* opts_mode, bool opts_size_given, double opts_size);

// bro.scene.SceneGraph.prototype.createCamera
//   registered at __bro_native.scene.SceneGraph_createCamera
//   opts_fov_given, opts_near_given, opts_far_given, opts_aspect_given, opts_mode_given, opts_size_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createCamera(void* self, bool opts_fov_given, double opts_fov, bool opts_near_given, double opts_near, bool opts_far_given, double opts_far, const double* opts_eye, uint32_t opts_eye_len, const double* opts_target, uint32_t opts_target_len, const double* opts_up, uint32_t opts_up_len, bool opts_aspect_given, double opts_aspect, const double* opts_quaternion, uint32_t opts_quaternion_len, bool opts_mode_given, const char* opts_mode, bool opts_size_given, double opts_size);

// bro.scene.SceneGraph.prototype.setActiveCamera
//   registered at __bro_native.scene.SceneGraph_setActiveCamera
void bro_scene_SceneGraph_setActiveCamera(void* self, void* camera);

// bro.scene.SceneGraph.prototype.setToneMap
//   registered at __bro_native.scene.SceneGraph_setToneMap
//   opts_mode_given, opts_whitePoint_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setToneMap(void* self, bool opts_mode_given, const char* opts_mode, bool opts_whitePoint_given, double opts_whitePoint);

// bro.scene.SceneGraph.prototype.setExposure
//   registered at __bro_native.scene.SceneGraph_setExposure
//   params: [mode (0 fixed, 1 auto), ev, compensation, speedUp, speedDown, minEv, maxEv, lowPercent, highPercent], NaN = default
void bro_scene_SceneGraph_setExposure(void* self, const double* params, uint32_t params_len);

// bro.scene.SceneGraph.prototype.setAmbient
//   registered at __bro_native.scene.SceneGraph_setAmbient
//   opts_intensity_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setAmbient(void* self, const double* opts_color, uint32_t opts_color_len, bool opts_intensity_given, double opts_intensity);

// bro.scene.SceneGraph.prototype.setWind
//   registered at __bro_native.scene.SceneGraph_setWind
//   opts_strength_given, opts_frequency_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setWind(void* self, const double* opts_direction, uint32_t opts_direction_len, bool opts_strength_given, double opts_strength, bool opts_frequency_given, double opts_frequency);

// bro.scene.SceneGraph.prototype.setWind (the wind-field half)
//   registered at __bro_native.scene.SceneGraph_setWindField
//   params: [speed, gust, gustSpeed, gustScale, drift]
void bro_scene_SceneGraph_setWindField(void* self, const double* params, uint32_t params_len);

// bro.scene.SceneGraph.prototype.windAt
//   registered at __bro_native.scene.SceneGraph_windAt
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_windAt(void* self, double x, double z, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.wind
//   registered at __bro_native.scene.SceneGraph_windState
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_windState(void* self, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.setShadowQuality
//   registered at __bro_native.scene.SceneGraph_setShadowQuality
//   opts_pcfTaps_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setShadowQuality(void* self, bool opts_pcfTaps_given, int32_t opts_pcfTaps);

// bro.scene.SceneGraph.prototype.setFog
//   registered at __bro_native.scene.SceneGraph_setFog
//   opts_mode_given, opts_density_given, opts_start_given, opts_end_given, opts_heightFalloff_given, opts_height_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setFog(void* self, bool opts_mode_given, const char* opts_mode, const double* opts_color, uint32_t opts_color_len, bool opts_density_given, double opts_density, bool opts_start_given, double opts_start, bool opts_end_given, double opts_end, bool opts_heightFalloff_given, double opts_heightFalloff, bool opts_height_given, double opts_height);

// bro.scene.SceneGraph.prototype.setAtmosphere
//   registered at __bro_native.scene.SceneGraph_setAtmosphere
//   params: 32 f64, NaN = Earth's default (the layout in js/scene_graph.js's setAtmosphere)
void bro_scene_SceneGraph_setAtmosphere(void* self, const double* params, uint32_t params_len);

// bro.scene.SceneGraph.prototype.setStarfield
//   registered at __bro_native.scene.SceneGraph_setStarfield
//   opts_starCount_given, opts_starSize_given, opts_twinkleSpeed_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setStarfield(void* self, bool opts_starCount_given, int32_t opts_starCount, bool opts_starSize_given, double opts_starSize, bool opts_twinkleSpeed_given, double opts_twinkleSpeed, const double* opts_tint, uint32_t opts_tint_len);

// bro.scene.SceneGraph.prototype.setTiltShift
//   registered at __bro_native.scene.SceneGraph_setTiltShift
//   opts_blur_given, opts_focus_given, opts_range_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setTiltShift(void* self, bool opts_blur_given, double opts_blur, bool opts_focus_given, double opts_focus, bool opts_range_given, double opts_range);

// bro.scene.SceneGraph.prototype.setBloom
//   registered at __bro_native.scene.SceneGraph_setBloom
//   opts_threshold_given, opts_intensity_given, opts_radius_given, opts_iterations_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setBloom(void* self, bool opts_threshold_given, double opts_threshold, bool opts_intensity_given, double opts_intensity, bool opts_radius_given, double opts_radius, bool opts_iterations_given, int32_t opts_iterations);

// bro.scene.SceneGraph.prototype.setSSAO
//   registered at __bro_native.scene.SceneGraph_setSSAO
//   opts_radius_given, opts_bias_given, opts_intensity_given, opts_sampleCount_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setSSAO(void* self, bool opts_radius_given, double opts_radius, bool opts_bias_given, double opts_bias, bool opts_intensity_given, double opts_intensity, bool opts_sampleCount_given, int32_t opts_sampleCount);

// bro.scene.SceneGraph.prototype.setSSR
//   registered at __bro_native.scene.SceneGraph_setSSR
//   the JS wrapper fills every default; enabled false turns SSR off and ignores the rest
void bro_scene_SceneGraph_setSSR(void* self, bool enabled, double maxDistance, int32_t steps, double thickness, double intensity, double edgeFade);

// bro.scene.SceneGraph.prototype.setDepthOfField
//   registered at __bro_native.scene.SceneGraph_setDepthOfField
//   opts_focusDistance_given, opts_focalLength_given, opts_fStop_given, opts_maxBlur_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setDepthOfField(void* self, bool opts_focusDistance_given, double opts_focusDistance, bool opts_focalLength_given, double opts_focalLength, bool opts_fStop_given, double opts_fStop, bool opts_maxBlur_given, double opts_maxBlur);

// bro.scene.SceneGraph.prototype.setColorLUT
//   registered at __bro_native.scene.SceneGraph_setColorLUT
bool bro_scene_SceneGraph_setColorLUT(void* self, const char* opts_path, int32_t opts_size, double opts_amount);

// bro.scene.SceneGraph.prototype.setFXAA
//   registered at __bro_native.scene.SceneGraph_setFXAA
void bro_scene_SceneGraph_setFXAA(void* self, bool enabled);

// bro.scene.SceneGraph.prototype.setGroundBounce
//   registered at __bro_native.scene.SceneGraph_setGroundBounce
void bro_scene_SceneGraph_setGroundBounce(void* self, double r, double g, double b);

// bro.scene.SceneGraph.prototype.setProbeGrid
//   registered at __bro_native.scene.SceneGraph_setProbeGrid
void bro_scene_SceneGraph_setProbeGrid(void* self, bool enabled);

// bro.scene.SceneGraph.prototype.setDither
//   registered at __bro_native.scene.SceneGraph_setDither
void bro_scene_SceneGraph_setDither(void* self, bool enabled);

// bro.scene.SceneGraph.prototype.setFrameTarget
//   registered at __bro_native.scene.SceneGraph_setFrameTarget
void bro_scene_SceneGraph_setFrameTarget(void* self, double ms);

// bro.scene.SceneGraph.prototype.setRenderScale
//   registered at __bro_native.scene.SceneGraph_setRenderScale
void bro_scene_SceneGraph_setRenderScale(void* self, double scale);

// bro.scene.SceneGraph.prototype.setMSAA
//   registered at __bro_native.scene.SceneGraph_setMSAA
void bro_scene_SceneGraph_setMSAA(void* self, int32_t samples);

// bro.scene.SceneGraph.prototype.setEnvironment
//   registered at __bro_native.scene.SceneGraph_setEnvironment
//   opts_panorama_given, opts_cubeMap_given, opts_intensity_given, opts_blur_given, opts_background_given: false when the value was not passed (no declared default)
//   returns false when a given panorama does not load (the previous environment stays)
bool bro_scene_SceneGraph_setEnvironment(void* self, bool opts_panorama_given, const char* opts_panorama, bool opts_cubeMap_given, const char* opts_cubeMap, const double* opts_color, uint32_t opts_color_len, bool opts_intensity_given, double opts_intensity, bool opts_blur_given, double opts_blur, bool opts_background_given, bool opts_background);

// bro.scene.SceneGraph.prototype.setFrustumCulling
//   registered at __bro_native.scene.SceneGraph_setFrustumCulling
void bro_scene_SceneGraph_setFrustumCulling(void* self, bool enabled);

// bro.scene.SceneGraph.prototype.cullStats: runs the operation and keeps its SceneCullStats result in a per-thread slot the SceneGraph_cullStats_<member> reads answer from until the next call
//   registered at __bro_native.scene.SceneGraph_cullStats
void bro_scene_SceneGraph_cullStats(void* self);

// SceneCullStats.totalNodes of the result SceneGraph_cullStats kept
//   registered at __bro_native.scene.SceneGraph_cullStats_totalNodes
int32_t bro_scene_SceneGraph_cullStats_totalNodes(void);

// SceneCullStats.renderedNodes of the result SceneGraph_cullStats kept
//   registered at __bro_native.scene.SceneGraph_cullStats_renderedNodes
int32_t bro_scene_SceneGraph_cullStats_renderedNodes(void);

// SceneCullStats.culledNodes of the result SceneGraph_cullStats kept
//   registered at __bro_native.scene.SceneGraph_cullStats_culledNodes
int32_t bro_scene_SceneGraph_cullStats_culledNodes(void);

// bro.scene.SceneGraph.prototype.clear
//   registered at __bro_native.scene.SceneGraph_clear
void bro_scene_SceneGraph_clear(void* self);

// bro.scene.SceneGraph.prototype.syncPhysics
//   registered at __bro_native.scene.SceneGraph_syncPhysics
void bro_scene_SceneGraph_syncPhysics(void* self);

// bro.scene.SceneGraph.prototype.raycast: runs the operation and keeps its SceneRaycastResult result in a per-thread slot the SceneGraph_raycast_<member> reads answer from until the next call; false = null
//   registered at __bro_native.scene.SceneGraph_raycast
bool bro_scene_SceneGraph_raycast(void* self, const double* origin, uint32_t origin_len, const double* direction, uint32_t direction_len);

// SceneRaycastResult.node of the result SceneGraph_raycast kept
//   registered at __bro_native.scene.SceneGraph_raycast_node
void* bro_scene_SceneGraph_raycast_node(void);

// SceneRaycastResult.point of the result SceneGraph_raycast kept
//   registered at __bro_native.scene.SceneGraph_raycast_point
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_raycast_point(bronze_native_buffer* out);

// SceneRaycastResult.normal of the result SceneGraph_raycast kept
//   registered at __bro_native.scene.SceneGraph_raycast_normal
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_raycast_normal(bronze_native_buffer* out);

// SceneRaycastResult.distance of the result SceneGraph_raycast kept
//   registered at __bro_native.scene.SceneGraph_raycast_distance
double bro_scene_SceneGraph_raycast_distance(void);

// bro.scene.SceneGraph.prototype.unprojectLocal
//   registered at __bro_native.scene.SceneGraph_unprojectLocal
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneGraph_unprojectLocal(void* self, double x, double y, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.projectLocal
//   registered at __bro_native.scene.SceneGraph_projectLocal
//   result: f64[] in *out ([x, y, depth], or empty before the camera exists); out->release == NULL, the runtime copies
void bro_scene_SceneGraph_projectLocal(void* self, double x, double y, double z, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.bindAudioListenerToCamera
//   registered at __bro_native.scene.SceneGraph_bindAudioListenerToCamera
void bro_scene_SceneGraph_bindAudioListenerToCamera(void* self, bool bind);

// bro.scene.SceneGraph.prototype.detachAIWorld
//   registered at __bro_native.scene.SceneGraph_detachAIWorld
void bro_scene_SceneGraph_detachAIWorld(void* self);

// ---- Grass (native_scene_grass.cpp, js/scene_grass.js) ----

// bro.scene.SceneGraph.prototype.createGrass
//   registered at __bro_native.scene.SceneGraph_createGrass
void* bro_scene_SceneGraph_createGrass(void* self);

// grass.setField: cols x rows texels of (height, density, dry, height multiplier),
// texel (i, j) centred at world (x0 + i * cell, z0 + j * cell)
//   registered at __bro_native.scene.SceneNode_setGrassField
void bro_scene_SceneNode_setGrassField(void* self, const float* data, uint32_t data_len, int32_t cols, int32_t rows,
                                       double x0, double z0, double cell);

// grass params: width, density, range (GrassNode::paramsFromArray; a NaN keeps a value)
//   registered at __bro_native.scene.SceneNode_setGrassParams
void bro_scene_SceneNode_setGrassParams(void* self, const double* params, uint32_t params_len);

//   registered at __bro_native.scene.SceneNode_grassParams
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_grassParams(void* self, bronze_native_buffer* out);

// grass.addMover / removeMover (radius <= 0 removes); `height`: how high it
// reaches into shrubs and low foliage (m above a plant's root)
//   registered at __bro_native.scene.SceneNode_setGrassMover
void bro_scene_SceneNode_setGrassMover(void* self, void* target, double radius, double strength, double trample,
                                       double height);

// grass.push: a one-off push this frame
//   registered at __bro_native.scene.SceneNode_grassPush
void bro_scene_SceneNode_grassPush(void* self, double x, double z, double radius, double vx, double vz,
                                   double strength, double trample);

// grass.stats: [blade rings n, ring 0 instances .. ring n-1, movers, field cols, field rows,
//               herb ring 0 instances, herb ring 1, ...]
//   registered at __bro_native.scene.SceneNode_grassStats
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_grassStats(void* self, bronze_native_buffer* out);

// ---- Plants (native_scene_plants.cpp, js/scene_plants.js) ----

// bro.scene.SceneGraph.prototype.createPlants
//   registered at __bro_native.scene.SceneGraph_createPlants
void* bro_scene_SceneGraph_createPlants(void* self);

// plants module: the world's plant module's GLSL ("" none: nothing drawn)
//   registered at __bro_native.scene.SceneNode_setPlantModule
void bro_scene_SceneNode_setPlantModule(void* self, const char* source);

// plants species: PlantsNode::kSpeciesFloats floats a species
//   registered at __bro_native.scene.SceneNode_setPlantSpecies
void bro_scene_SceneNode_setPlantSpecies(void* self, const float* data, uint32_t data_len);

// plants: PlantsNode::kPlantFloats floats a plant
//   registered at __bro_native.scene.SceneNode_setPlants
void bro_scene_SceneNode_setPlants(void* self, const float* data, uint32_t data_len);

// plants rules: world/plant_placement.h's JSON (parsePlacementRules); empty
// clears them. Returns "" or what was wrong with them.
//   registered at __bro_native.scene.SceneNode_setPlantRules
const char* bro_scene_SceneNode_setPlantRules(void* self, const char* json);

// plants placement: [leaves placed, leaves wanted and not yet placed]
//   registered at __bro_native.scene.SceneNode_plantPlacement
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_plantPlacement(void* self, bronze_native_buffer* out);

// plants params: pixels, minElements, woodRange, range (a NaN keeps a value)
//   registered at __bro_native.scene.SceneNode_setPlantParams
void bro_scene_SceneNode_setPlantParams(void* self, const double* params, uint32_t params_len);

// plants stats: [plants, plants in view, elements drawn, limbs drawn]; empty
// for a node that is not a plants node
//   registered at __bro_native.scene.SceneNode_plantStats
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_plantStats(void* self, bronze_native_buffer* out);

// grass floor: one float a field texel (0 open .. 1 a closed canopy), or two
// (that, and the herbs' density 0..1); fewer than the field's texels clears it
//   registered at __bro_native.scene.SceneNode_setGrassFloor
void bro_scene_SceneNode_setGrassFloor(void* self, const float* data, uint32_t data_len);

// grass herbs: a herb module's GLSL ("" clears them), the data its
// herbData(i) reads (at most 1024 floats), its tallest plant (m), the range
// the whole plants are drawn to and the far plants' (m; 0 keeps it)
//   registered at __bro_native.scene.SceneNode_setGrassHerbs
void bro_scene_SceneNode_setGrassHerbs(void* self, const char* source, const float* data, uint32_t data_len,
                                       double height, double range, double far);

// grass sward: a sward module's GLSL ("" clears it), the data its swardData(i)
// reads (at most 256 floats), its blades' mean height, the tallest blade and
// how far a blade reaches from its root (m)
//   registered at __bro_native.scene.SceneNode_setGrassSward
void bro_scene_SceneNode_setGrassSward(void* self, const char* source, const float* data, uint32_t data_len,
                                       double height, double tallest, double reach);

// mesh.turf: shade a mesh node as grass node `grass`'s ground (on 0: as itself)
//   registered at __bro_native.scene.SceneNode_setTurfGrass
void bro_scene_SceneNode_setTurfGrass(void* self, void* grass, int32_t on);

// node.sampleHeights: a mesh node's top surface (world y) on a grid, NaN where
// it has none; texel (i, j) at world (x0 + i * cell, z0 + j * cell)
//   registered at __bro_native.scene.SceneNode_sampleHeights
//   result: f32[] in *out; out->release set, the array is a view over it
void bro_scene_SceneNode_sampleHeights(void* self, double x0, double z0, double cell, int32_t cols, int32_t rows,
                                       bronze_native_buffer* out);

// node.worldBounds for a mesh node: [minX, minY, minZ, maxX, maxY, maxZ], empty when none
//   registered at __bro_native.scene.SceneNode_meshWorldBounds
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_scene_SceneNode_meshWorldBounds(void* self, bronze_native_buffer* out);

// Registered in native_scene_register.cpp as well; the bodies are in the native_*.cpp files.
void bro_scene_SceneGraph_attachTo(uint64_t sceneBits, uint64_t canvasBits);
void bro_scene_SceneGraph_detach(void* self);
bool bro_scene_SceneGraph_attached_get(void* self);
bool bro_scene_SceneGraph_keepAlive_get(void* self);
void bro_scene_SceneGraph_keepAlive_set(void* self, bool v);
bool bro_scene_SceneNode_setShaderTexture(void* self, const char* name, int32_t x, int32_t y, int32_t width, int32_t height, const float* data, uint32_t dataCount, bool mipmap, bool isSub);
void bro_scene_SceneNode_setBaseColorTextureData(void* self, int32_t width, int32_t height, const uint8_t* data, uint32_t len);
void bro_scene_SceneNode_clearBaseColorTexture(void* self);
void bro_scene_SceneNode_setEmissionTextureData(void* self, int32_t width, int32_t height, const uint8_t* data, uint32_t len);
void bro_scene_SceneNode_onAnimationEnd_set(void* self, uint64_t cbBits);
int32_t bro_scene_SceneNode_renderPriority_get(void* self);
void bro_scene_SceneNode_renderPriority_set(void* self, int32_t p);
double bro_scene_SceneNode_emissionStrength_get(void* self);
void bro_scene_SceneNode_emissionStrength_set(void* self, double s);
double bro_scene_SceneNode_upperFade_get(void* self);
void bro_scene_SceneNode_upperFade_set(void* self, double f);
double bro_scene_SceneNode_lowerFade_get(void* self);
void bro_scene_SceneNode_lowerFade_set(void* self, double f);
double bro_scene_SceneNode_normalFade_get(void* self);
void bro_scene_SceneNode_normalFade_set(void* self, double f);
void bro_scene_SceneNode_modulate_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_modulate_set(void* self, const double* data, uint32_t len);
int32_t bro_scene_SceneNode_particleCount_get(void* self);
bool bro_scene_SceneNode_particlePlaying_get(void* self);
double bro_scene_SceneNode_particleRate_get(void* self);
void bro_scene_SceneNode_particleRate_set(void* self, double r);
void bro_scene_SceneNode_particleEmit(void* self, const double* d, uint32_t len);
bool bro_scene_SceneNode_particlePaused_get(void* self);
double bro_scene_SceneNode_softness_get(void* self);
void bro_scene_SceneNode_softness_set(void* self, double s);
void bro_scene_SceneNode_onFinished_set(void* self, uint64_t cbBits);
void bro_scene_SceneGraph_clearActiveCamera(void* self);
int32_t bro_scene_SceneGraph_raycast_instance(void);
double bro_scene_SceneNode_alphaCutoff_get(void* self);
void bro_scene_SceneNode_alphaCutoff_set(void* self, double v);
bool bro_scene_SceneNode_doubleSided_get(void* self);
void bro_scene_SceneNode_doubleSided_set(void* self, bool v);
double bro_scene_SceneNode_interior_get(void* self);
void bro_scene_SceneNode_interior_set(void* self, double v);
int32_t bro_scene_SceneNode_priority_get(void* self);
void bro_scene_SceneNode_priority_set(void* self, int32_t v);
void bro_scene_SceneNode_emissiveColor_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_emissiveColor_set(void* self, const double* v, uint32_t len);
void bro_scene_SceneNode_emissiveColorCss_set(void* self, const char* css);
double bro_scene_SceneNode_nearClipDist_get(void* self);
void bro_scene_SceneNode_nearClipDist_set(void* self, double v);
double bro_scene_SceneNode_radius_get(void* self);
void bro_scene_SceneNode_radius_set(void* self, double v);
const char* bro_scene_SceneNode_fillColor_get(void* self);
void bro_scene_SceneNode_fillColor_set(void* self, const char* css);
const char* bro_scene_SceneNode_strokeColor_get(void* self);
void bro_scene_SceneNode_strokeColor_set(void* self, const char* css);
double bro_scene_SceneNode_strokeWidth_get(void* self);
void bro_scene_SceneNode_strokeWidth_set(void* self, double v);
bool bro_scene_SceneNode_autoSync_get(void* self);
void bro_scene_SceneNode_autoSync_set(void* self, bool v);
double bro_scene_SceneNode_pixelsPerUnit_get(void* self);
void bro_scene_SceneNode_pixelsPerUnit_set(void* self, double v);
void bro_scene_SceneNode_syncToPhysics(void* self);
const char* bro_scene_SceneNode_billboard_get(void* self);
void bro_scene_SceneNode_billboard_set(void* self, const char* mode);
int32_t bro_scene_SceneNode_frameIndex_get(void* self);
void bro_scene_SceneNode_frameIndex_set(void* self, int32_t v);
void bro_scene_SceneNode_updateInstance(void* self, int32_t index, const float* data, uint32_t len);
void bro_scene_SceneNode_setInstancedMesh(void* self, uint64_t meshBits);
void bro_scene_SceneNode_setAtlasGrid(void* self, int32_t cols, int32_t rows);
void bro_scene_SceneNode_setScatterSegments(void* self, const float* seg, uint32_t segLen, const float* instSeg, uint32_t instLen, const double* params, uint32_t paramLen, const float* bounds, uint32_t boundsLen);
void bro_scene_SceneNode_setTubeSegments(void* self, const float* seg, uint32_t segLen, int32_t sides, double radiusScale, const float* bounds, uint32_t boundsLen, const float* tangents, uint32_t tanLen);
void bro_scene_SceneNode_setVegetation(void* self, const float* data, uint32_t len);
bool bro_scene_SceneNode_isScatter(void* self);
bool bro_scene_SceneNode_isTube(void* self);
double bro_scene_SceneNode_anchorX_get(void* self);
double bro_scene_SceneNode_anchorY_get(void* self);
void bro_scene_SceneNode_anchor_set(void* self, double ax, double ay);
double bro_scene_SceneNode_cornerRadius_get(void* self);
void bro_scene_SceneNode_cornerRadius_set(void* self, double v);
double bro_scene_SceneNode_pxPerUnit_get(void* self);
void bro_scene_SceneNode_pxPerUnit_set(void* self, double v);
double bro_scene_SceneNode_bodyId_get(void* self);
int32_t bro_scene_SceneNode_atlasCols_get(void* self);
int32_t bro_scene_SceneNode_atlasRows_get(void* self);
bool bro_scene_SceneNode_staticBatch_get(void* self);
void bro_scene_SceneNode_staticBatch_set(void* self, bool v);
void bro_scene_SceneGraph_render(void* self);
double bro_scene_SceneGraph_canvasWidth(void* self);
double bro_scene_SceneGraph_canvasHeight(void* self);
void bro_scene_SceneGraph_setCanvasSize(void* self, double w, double h);
void bro_scene_SceneGraph_readTonemapPixels(void* self, bronze_native_buffer* out);
int32_t bro_scene_SceneGraph_readTonemapWidth(void);
int32_t bro_scene_SceneGraph_readTonemapHeight(void);
void* bro_scene_SceneGraph_createMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createSkinnedMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createInstancedMesh(void* self, uint64_t optsBits, uint64_t meshHandle);
void* bro_scene_SceneGraph_createShape(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createSprite(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createPhysicsNode(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createParticles3D(void* self, const char* jsonOpts);
void* bro_scene_SceneGraph_createGaussianSplat(void* self, const char* jsonOpts);
const char* bro_scene_SceneNode_type_get(void* self);
const char* bro_scene_SceneNode_kind_get(void* self);
int32_t bro_scene_SceneNode_childCount_get(void* self);
bool bro_scene_SceneNode_castsShadow_get(void* self);
void bro_scene_SceneNode_castsShadow_set(void* self, bool v);
bool bro_scene_SceneNode_receivesShadow_get(void* self);
void bro_scene_SceneNode_receivesShadow_set(void* self, bool v);
double bro_scene_SceneNode_metallic_get(void* self);
void bro_scene_SceneNode_metallic_set(void* self, double v);
double bro_scene_SceneNode_roughness_get(void* self);
void bro_scene_SceneNode_roughness_set(void* self, double v);
double bro_scene_SceneNode_emissive_get(void* self);
void bro_scene_SceneNode_emissive_set(void* self, double v);
void bro_scene_SceneNode_direction_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_direction_set(void* self, const double* v, uint32_t len);
void bro_scene_SceneNode_color_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_color_set(void* self, const double* v, uint32_t len);
double bro_scene_SceneNode_intensity_get(void* self);
void bro_scene_SceneNode_intensity_set(void* self, double v);
double bro_scene_SceneNode_range_get(void* self);
void bro_scene_SceneNode_range_set(void* self, double v);
double bro_scene_SceneNode_innerAngle_get(void* self);
void bro_scene_SceneNode_innerAngle_set(void* self, double v);
double bro_scene_SceneNode_outerAngle_get(void* self);
void bro_scene_SceneNode_outerAngle_set(void* self, double v);
double bro_scene_SceneNode_shadowBias_get(void* self);
void bro_scene_SceneNode_shadowBias_set(void* self, double v);
double bro_scene_SceneNode_shadowNormalBias_get(void* self);
void bro_scene_SceneNode_shadowNormalBias_set(void* self, double v);
double bro_scene_SceneNode_shadowDistance_get(void* self);
void bro_scene_SceneNode_shadowDistance_set(void* self, double v);
void bro_scene_SceneNode_setSkeleton(void* self, uint64_t skelVal);
void bro_scene_SceneNode_addClip(void* self, const char* name, uint64_t clipVal);
void bro_scene_SceneNode_addBlendSpace1D(void* self, const char* name, const char* jsonPoints);
void bro_scene_SceneNode_addBlendSpace2D(void* self, const char* name, const char* jsonPoints);
void bro_scene_SceneNode_setBlendPos(void* self, const char* name, double x, bool hasY, double y);
void bro_scene_SceneNode_playLayer(void* self, int32_t layerIndex, const char* clipName, const char* jsonOpts);
void bro_scene_SceneNode_stopLayer(void* self, int32_t layerIndex, bool hasFade, double fadeTime);
void bro_scene_SceneNode_setLayerWeight(void* self, int32_t layerIndex, double weight);
void bro_scene_SceneNode_addStateMachine(void* self, const char* jsonDef);
void bro_scene_SceneNode_travel(void* self, const char* targetState);
void bro_scene_SceneNode_setRootMotion(void* self, const char* jsonOpts);
const char* bro_scene_SceneNode_consumeRootMotion(void* self);
void bro_scene_SceneNode_play(void* self, const char* clipName, const char* jsonOpts);
void bro_scene_SceneNode_stop(void* self, const char* jsonOpts);
void bro_scene_SceneNode_pause(void* self);
void bro_scene_SceneNode_resume(void* self);
int32_t bro_scene_SceneNode_setSkinningMatrices(void* self, uint64_t matsBits);
uint64_t bro_scene_SceneNode_getBoneWorldMatrix(void* self, uint64_t argBits);
const char* bro_scene_SceneNode_blendState(void* self);
void bro_scene_SceneNode_onAnimationFinished_set(void* self, uint64_t cbBits);
void bro_scene_SceneNode_onStateChanged_set(void* self, uint64_t cbBits);
bool bro_scene_SceneNode_hasShader(void* self);
const char* bro_scene_SceneNode_setShader(void* self, const char* vertex, const char* fragment,
                                          const char* uniformsJson);
void bro_scene_SceneNode_clearShader(void* self);
void bro_scene_SceneNode_setShaderUniform(void* self, const char* name, const double* vals, uint32_t count);
void bro_scene_SceneNode_setLodMeshes(void* self, const char* jsonLods);
void bro_scene_SceneNode_updateMesh(void* self, uint64_t meshBits, bool recomputeNormals);
int32_t bro_scene_SceneNode_lodCount(void* self);
int32_t bro_scene_SceneNode_lodLevel(void* self);
void bro_scene_SceneNode_visibilityRange_set(void* self, double begin, double end, double margin);
void bro_scene_SceneNode_visibilityRange_clear(void* self);
void bro_scene_SceneNode_visibilityRange_get(void* self, bronze_native_buffer* out);
bool bro_scene_SceneGraph_isValid(void* self);
void bro_scene_SceneNode_setBaseColorTextureFromScene(void* self, void* sourceScene);
const char* bro_scene_SceneGraph_cullStatsJson(void* self);
void bro_scene_SceneNode_setInstances(void* self, const float* data, uint32_t count);
void bro_scene_SceneNode_setInstancesFromTransforms(void* self, const float* data, uint32_t count);
double bro_scene_SceneNode_instanceCount_get(void* self);
const char* bro_scene_SceneNode_updateMode_get(void* self);
void bro_scene_SceneNode_updateMode_set(void* self, const char* mode);
int32_t bro_scene_SceneNode_resolution_get(void* self);
void bro_scene_SceneNode_resolution_set(void* self, int32_t res);
bool bro_scene_SceneNode_boxProjection_get(void* self);
void bro_scene_SceneNode_boxProjection_set(void* self, bool bp);
double bro_scene_SceneNode_cullMargin_get(void* self);
void bro_scene_SceneNode_cullMargin_set(void* self, double v);
int32_t bro_scene_SceneNode_splatCount_get(void* self);
void bro_scene_SceneNode_setCloud(void* self, const float* pos, uint32_t posCount, const float* scales, uint32_t scaleCount, const float* rots, uint32_t rotCount, const float* opacities, uint32_t opCount, const float* sh, uint32_t shCount, int32_t shDegree);
bool bro_scene_SceneNode_loadSplatPly(void* self, const char* path);
void bro_scene_SceneNode_worldAnchor_get(void* self, bronze_native_buffer* out);
void bro_scene_SceneNode_worldAnchor_set(void* self, const double* v, uint32_t len);

#ifdef __cplusplus
}  // extern "C"
#endif
