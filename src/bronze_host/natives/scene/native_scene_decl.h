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

// bro.scene.SceneGraph.prototype.shadowCache read
//   registered at __bro_native.scene.SceneGraph_shadowCache_get
bool bro_scene_SceneGraph_shadowCache_get(void* self);

// bro.scene.SceneGraph.prototype.shadowCache write
//   registered at __bro_native.scene.SceneGraph_shadowCache_set
void bro_scene_SceneGraph_shadowCache_set(void* self, bool v);

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
//   opts_maxParticles_given, opts_texture_given, opts_visible_given: false when the value was not passed (no declared default)
void* bro_scene_SceneGraph_createParticles(void* self, bool opts_maxParticles_given, int32_t opts_maxParticles, bool opts_texture_given, const char* opts_texture, const double* opts_position, uint32_t opts_position_len, bool opts_visible_given, bool opts_visible);

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
//   opts_mode_given, opts_exposure_given, opts_whitePoint_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setToneMap(void* self, bool opts_mode_given, const char* opts_mode, bool opts_exposure_given, double opts_exposure, bool opts_whitePoint_given, double opts_whitePoint);

// bro.scene.SceneGraph.prototype.setAmbient
//   registered at __bro_native.scene.SceneGraph_setAmbient
//   opts_intensity_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setAmbient(void* self, const double* opts_color, uint32_t opts_color_len, bool opts_intensity_given, double opts_intensity);

// bro.scene.SceneGraph.prototype.setWind
//   registered at __bro_native.scene.SceneGraph_setWind
//   opts_strength_given, opts_frequency_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setWind(void* self, const double* opts_direction, uint32_t opts_direction_len, bool opts_strength_given, double opts_strength, bool opts_frequency_given, double opts_frequency);

// bro.scene.SceneGraph.prototype.setShadowQuality
//   registered at __bro_native.scene.SceneGraph_setShadowQuality
//   opts_atlasSize_given, opts_pcfTaps_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setShadowQuality(void* self, bool opts_atlasSize_given, int32_t opts_atlasSize, bool opts_pcfTaps_given, int32_t opts_pcfTaps);

// bro.scene.SceneGraph.prototype.setShadowCache
//   registered at __bro_native.scene.SceneGraph_setShadowCache
//   opts_enabled_given, opts_staticResolution_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setShadowCache(void* self, bool opts_enabled_given, bool opts_enabled, bool opts_staticResolution_given, int32_t opts_staticResolution);

// bro.scene.SceneGraph.prototype.setFog
//   registered at __bro_native.scene.SceneGraph_setFog
//   opts_mode_given, opts_density_given, opts_start_given, opts_end_given, opts_heightFalloff_given, opts_height_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setFog(void* self, bool opts_mode_given, const char* opts_mode, const double* opts_color, uint32_t opts_color_len, bool opts_density_given, double opts_density, bool opts_start_given, double opts_start, bool opts_end_given, double opts_end, bool opts_heightFalloff_given, double opts_heightFalloff, bool opts_height_given, double opts_height);

// bro.scene.SceneGraph.prototype.setAtmosphere
//   registered at __bro_native.scene.SceneGraph_setAtmosphere
//   opts_turbidity_given, opts_sunIntensity_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setAtmosphere(void* self, const double* opts_rayleigh, uint32_t opts_rayleigh_len, const double* opts_mie, uint32_t opts_mie_len, bool opts_turbidity_given, double opts_turbidity, const double* opts_sunPosition, uint32_t opts_sunPosition_len, bool opts_sunIntensity_given, double opts_sunIntensity);

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
//   opts_maxDistance_given, opts_thickness_given, opts_stepCount_given, opts_roughnessCutoff_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setSSR(void* self, bool opts_maxDistance_given, double opts_maxDistance, bool opts_thickness_given, double opts_thickness, bool opts_stepCount_given, int32_t opts_stepCount, bool opts_roughnessCutoff_given, double opts_roughnessCutoff);

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

// bro.scene.SceneGraph.prototype.setRenderScale
//   registered at __bro_native.scene.SceneGraph_setRenderScale
void bro_scene_SceneGraph_setRenderScale(void* self, double scale);

// bro.scene.SceneGraph.prototype.setMSAA
//   registered at __bro_native.scene.SceneGraph_setMSAA
void bro_scene_SceneGraph_setMSAA(void* self, int32_t samples);

// bro.scene.SceneGraph.prototype.setEnvironment
//   registered at __bro_native.scene.SceneGraph_setEnvironment
//   opts_panorama_given, opts_cubeMap_given, opts_intensity_given, opts_blur_given, opts_background_given: false when the value was not passed (no declared default)
void bro_scene_SceneGraph_setEnvironment(void* self, bool opts_panorama_given, const char* opts_panorama, bool opts_cubeMap_given, const char* opts_cubeMap, const double* opts_color, uint32_t opts_color_len, bool opts_intensity_given, double opts_intensity, bool opts_blur_given, double opts_blur, bool opts_background_given, bool opts_background);

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
void bro_scene_SceneGraph_unprojectLocal(void* self, void* node, const double* screenPoint, uint32_t screenPoint_len, bronze_native_buffer* out);

// bro.scene.SceneGraph.prototype.bindAudioListenerToCamera
//   registered at __bro_native.scene.SceneGraph_bindAudioListenerToCamera
void bro_scene_SceneGraph_bindAudioListenerToCamera(void* self, bool bind);

// bro.scene.SceneGraph.prototype.detachAIWorld
//   registered at __bro_native.scene.SceneGraph_detachAIWorld
void bro_scene_SceneGraph_detachAIWorld(void* self);

#ifdef __cplusplus
}  // extern "C"
#endif
