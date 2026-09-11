// =============================================================================
// bro_scene_c_abi.h — Pure C-ABI declarations for bro.scene
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_SCENE_C_ABI_H
#define BRO_SCENE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.scene.SceneNode ---
void* bro_SceneNode_create(void);
void  bro_SceneNode_destroy(void* self);
int32_t bro_SceneNode_get_id(void* self);
const char* bro_SceneNode_get_name(void* self);
void bro_SceneNode_set_name(void* self, const char* val);
bool bro_SceneNode_get_visible(void* self);
void bro_SceneNode_set_visible(void* self, bool val);
void* bro_SceneNode_get_position(void* self);
void bro_SceneNode_set_position(void* self, void* val);
void* bro_SceneNode_get_rotation(void* self);
void bro_SceneNode_set_rotation(void* self, void* val);
void* bro_SceneNode_get_scale(void* self);
void bro_SceneNode_set_scale(void* self, void* val);
void* bro_SceneNode_get_worldPosition(void* self);
void* bro_SceneNode_get_worldMatrix(void* self);
void* bro_SceneNode_get_parent(void* self);
void* bro_SceneNode_get_children(void* self);
void* bro_SceneNode_add(void* self, void* child);
void bro_SceneNode_remove(void* self, void* child);
void* bro_SceneNode_addChild(void* self, void* child);
void bro_SceneNode_removeChild(void* self, void* child);
void bro_SceneNode_destroy(void* self);
void* bro_SceneNode_setPosition(void* self, double x, double y, double z);
void* bro_SceneNode_setRotation(void* self, double x, double y, double z, double w);
void* bro_SceneNode_setScale(void* self, double x, double y, double z);
void* bro_SceneNode_lookAt(void* self, void* target, void* up);
void* bro_SceneNode_setMaterial(void* self, void* mat);
void* bro_SceneNode_setSkeleton(void* self, void* skeleton);
void* bro_SceneNode_addClip(void* self, const char* name, void* anim);
void* bro_SceneNode_getBoneWorldMatrix(void* self, void* nameOrIndex);
void* bro_SceneNode_addBlendSpace1D(void* self, const char* name, void* clips);
void* bro_SceneNode_addBlendSpace2D(void* self, const char* name, void* clips);
void* bro_SceneNode_setBlendPos(void* self, const char* name, double x, double y);
void* bro_SceneNode_blendState(void* self, const char* name);
void* bro_SceneNode_playLayer(void* self, int32_t layer, const char* clipName, double weight, double fadeTime);
void* bro_SceneNode_stopLayer(void* self, int32_t layer, double fadeTime);
void* bro_SceneNode_setLayerWeight(void* self, int32_t layer, double weight);
void* bro_SceneNode_addStateMachine(void* self, const char* name, void* def);
bool bro_SceneNode_travel(void* self, const char* name, const char* targetState);
void* bro_SceneNode_setRootMotion(void* self, bool enabled);
void* bro_SceneNode_consumeRootMotion(void* self);
void* bro_SceneNode_play(void* self, const char* clipName, void* opts);
void* bro_SceneNode_stop(void* self);
void* bro_SceneNode_pause(void* self);
void* bro_SceneNode_resume(void* self);
void bro_SceneNode_setInstanceTransform(void* self, int32_t index, void* matrix);
void bro_SceneNode_setInstanceColor(void* self, int32_t index, void* color);
void bro_SceneNode_setInstanceCount(void* self, int32_t count);
void* bro_SceneNode_setHtml(void* self, const char* html);
void bro_SceneNode_markHtmlDirty(void* self);
void bro_SceneNode_burst(void* self, int32_t count);
void bro_SceneNode_clear(void* self);
void bro_SceneNode_probeCapture(void* self);
void bro_SceneNode_savePly(void* self, const char* path);

// --- Interface bro.scene.SceneGraph ---
void* bro_SceneGraph_create(void);
void  bro_SceneGraph_destroy(void* self);
void* bro_SceneGraph_get_root(void* self);
double bro_SceneGraph_get_cameraX(void* self);
void bro_SceneGraph_set_cameraX(void* self, double val);
double bro_SceneGraph_get_cameraY(void* self);
void bro_SceneGraph_set_cameraY(void* self, double val);
double bro_SceneGraph_get_cameraZoom(void* self);
void bro_SceneGraph_set_cameraZoom(void* self, double val);
bool bro_SceneGraph_get_showLightIcons(void* self);
void bro_SceneGraph_set_showLightIcons(void* self, bool val);
bool bro_SceneGraph_get_frustumCulling(void* self);
void bro_SceneGraph_set_frustumCulling(void* self, bool val);
bool bro_SceneGraph_get_shadowCache(void* self);
void bro_SceneGraph_set_shadowCache(void* self, bool val);
double bro_SceneGraph_get_renderScale(void* self);
void bro_SceneGraph_set_renderScale(void* self, double val);
double bro_SceneGraph_get_msaa(void* self);
void bro_SceneGraph_set_msaa(void* self, double val);
void* bro_SceneGraph_get_activeCamera(void* self);
void bro_SceneGraph_set_activeCamera(void* self, void* val);
void* bro_SceneGraph_get_viewMatrix(void* self);
void* bro_SceneGraph_get_projectionMatrix(void* self);
void* bro_SceneGraph_get_cameraEye(void* self);
void* bro_SceneGraph_createNode(void* self, void* opts);
void* bro_SceneGraph_createShape(void* self, void* opts);
void* bro_SceneGraph_createSprite(void* self, void* opts);
void* bro_SceneGraph_createPhysicsNode(void* self, void* opts);
void* bro_SceneGraph_createMesh(void* self, void* opts);
void* bro_SceneGraph_createSkinnedMesh(void* self, void* opts);
void* bro_SceneGraph_createInstancedMesh(void* self, void* opts);
void* bro_SceneGraph_createGaussianSplat(void* self, void* opts);
void* bro_SceneGraph_createHtmlNode(void* self, void* opts);
void* bro_SceneGraph_createLight(void* self, void* opts);
void* bro_SceneGraph_createParticles(void* self, void* opts);
void* bro_SceneGraph_createParticles3D(void* self, void* opts);
void* bro_SceneGraph_createDecal(void* self, void* opts);
void* bro_SceneGraph_createReflectionProbe(void* self, void* opts);
void* bro_SceneGraph_createTween(void* self);
void* bro_SceneGraph_createAnimationPlayer(void* self);
void* bro_SceneGraph_createTerrain(void* self, void* opts);
void* bro_SceneGraph_createClipmapTerrain(void* self, void* opts);
void* bro_SceneGraph_createTileWorld(void* self, void* opts);
void* bro_SceneGraph_findById(void* self, int32_t id);
void* bro_SceneGraph_findByName(void* self, const char* name);
void bro_SceneGraph_destroyNode(void* self, void* node);
void bro_SceneGraph_setCamera(void* self, void* opts);
void* bro_SceneGraph_createCamera(void* self, void* opts);
void bro_SceneGraph_setActiveCamera(void* self, void* camera);
void bro_SceneGraph_setToneMap(void* self, void* opts);
void bro_SceneGraph_setAmbient(void* self, void* opts);
void bro_SceneGraph_setWind(void* self, void* dir, double speed);
void bro_SceneGraph_setShadowQuality(void* self, void* opts);
void bro_SceneGraph_setShadowCache(void* self, void* opts);
void bro_SceneGraph_setFog(void* self, void* opts);
void bro_SceneGraph_setAtmosphere(void* self, void* opts);
void bro_SceneGraph_setStarfield(void* self, void* opts);
void bro_SceneGraph_setTiltShift(void* self, void* opts);
void bro_SceneGraph_setBloom(void* self, void* opts);
void bro_SceneGraph_setSSAO(void* self, void* opts);
void bro_SceneGraph_setSSR(void* self, void* opts);
void bro_SceneGraph_setDepthOfField(void* self, void* opts);
void bro_SceneGraph_setColorLUT(void* self, void* opts);
void bro_SceneGraph_setFXAA(void* self, bool enabled);
void bro_SceneGraph_setRenderScale(void* self, double scale);
void bro_SceneGraph_setMSAA(void* self, int32_t samples);
void bro_SceneGraph_setEnvironment(void* self, void* opts);
void bro_SceneGraph_setFrustumCulling(void* self, bool enabled);
void* bro_SceneGraph_cullStats(void* self);
void bro_SceneGraph_clear(void* self);
void bro_SceneGraph_syncPhysics(void* self);
void* bro_SceneGraph_raycast(void* self, void* origin, void* direction);
void* bro_SceneGraph_unprojectLocal(void* self, void* node, void* screenPoint);
void* bro_SceneGraph_toImageData(void* self);
void* bro_SceneGraph_captureFrame(void* self, const char* format, double quality);
void* bro_SceneGraph_asTexture(void* self);
void bro_SceneGraph_bindAudioListenerToCamera(void* self, bool bind);
void bro_SceneGraph_attachAIWorld(void* self, void* aiWorld, void* opts);
void bro_SceneGraph_detachAIWorld(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_SCENE_C_ABI_H
