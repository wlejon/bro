// native_scene_register.cpp — registers every native of native_scene_decl.h with
// bronze (embed::registerNative) under __bro_native.scene. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_scene from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program, after registerNatives_animation, registerNatives_terrain.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.scene.<Class>Proto for js/scene.js to chain.

#include "native_scene_decl.h"
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params, ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

bool ctor(const char* path, void* f, ev::HandleDestructor dtor, bronze::runtime::Finalize finalize,
          std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s = sig(path, params, ev::NativeKind::Constructor);
    s.className = path;
    s.destructor = dtor;
    s.finalize = finalize;
    return ev::registerNative(path, f, s, error);
}

// The prototype every instance of a class is born on, published where the
// wrapper reads it. Skipped when the roots are not there (bro-native-manifest
// registers without them; the manifest does not need the prototype).
void publishPrototype(const char* ns, const char* className, const char* protoName) {
    ev::GlobalValue root = ev::globalValue("__bro_native");
    if (!root.found || !ev::isObject(root.value)) return;
    ev::GlobalValue proto = ev::nativeClassPrototype(className);
    if (!proto.found) return;
    // Each value rooted before the next allocating call (bronze embed GC contract).
    ev::Persistent rootSlot(root.value);
    ev::Persistent protoSlot(proto.value);
    ev::Persistent nsSlot(ev::getProperty(rootSlot.get(), ns));
    if (!ev::isObject(nsSlot.get())) return;
    ev::setProperty(nsSlot.get(), protoName, protoSlot.get());
}

}  // namespace

bool registerNatives_scene(std::string* error) {
    const bool ok =
        ctor("__bro_native.scene.SceneNode", BRO_NATIVE_FN(bro_scene_SceneNode_ctor), BRO_NATIVE_DTOR(bro_scene_SceneNode_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.scene.SceneGraph", BRO_NATIVE_FN(bro_scene_SceneGraph_ctor), BRO_NATIVE_DTOR(bro_scene_SceneGraph_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.scene.SceneNode_id_get", BRO_NATIVE_FN(bro_scene_SceneNode_id_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_name_get", BRO_NATIVE_FN(bro_scene_SceneNode_name_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_name_set", BRO_NATIVE_FN(bro_scene_SceneNode_name_set), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneNode_visible_get", BRO_NATIVE_FN(bro_scene_SceneNode_visible_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_visible_set", BRO_NATIVE_FN(bro_scene_SceneNode_visible_set), "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
        fn("__bro_native.scene.SceneNode_x_get", BRO_NATIVE_FN(bro_scene_SceneNode_x_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_x_set", BRO_NATIVE_FN(bro_scene_SceneNode_x_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_y_get", BRO_NATIVE_FN(bro_scene_SceneNode_y_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_y_set", BRO_NATIVE_FN(bro_scene_SceneNode_y_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_z_get", BRO_NATIVE_FN(bro_scene_SceneNode_z_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_z_set", BRO_NATIVE_FN(bro_scene_SceneNode_z_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_position_get", BRO_NATIVE_FN(bro_scene_SceneNode_position_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_position_set", BRO_NATIVE_FN(bro_scene_SceneNode_position_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_rotation_get", BRO_NATIVE_FN(bro_scene_SceneNode_rotation_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_rotation_set", BRO_NATIVE_FN(bro_scene_SceneNode_rotation_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationX_get", BRO_NATIVE_FN(bro_scene_SceneNode_rotationX_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationX_set", BRO_NATIVE_FN(bro_scene_SceneNode_rotationX_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationY_get", BRO_NATIVE_FN(bro_scene_SceneNode_rotationY_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationY_set", BRO_NATIVE_FN(bro_scene_SceneNode_rotationY_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationZ_get", BRO_NATIVE_FN(bro_scene_SceneNode_rotationZ_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_rotationZ_set", BRO_NATIVE_FN(bro_scene_SceneNode_rotationZ_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_scale_get", BRO_NATIVE_FN(bro_scene_SceneNode_scale_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_scale_set", BRO_NATIVE_FN(bro_scene_SceneNode_scale_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleX_get", BRO_NATIVE_FN(bro_scene_SceneNode_scaleX_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleX_set", BRO_NATIVE_FN(bro_scene_SceneNode_scaleX_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleY_get", BRO_NATIVE_FN(bro_scene_SceneNode_scaleY_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleY_set", BRO_NATIVE_FN(bro_scene_SceneNode_scaleY_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleZ_get", BRO_NATIVE_FN(bro_scene_SceneNode_scaleZ_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_scaleZ_set", BRO_NATIVE_FN(bro_scene_SceneNode_scaleZ_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_quaternion_get", BRO_NATIVE_FN(bro_scene_SceneNode_quaternion_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_quaternion_set", BRO_NATIVE_FN(bro_scene_SceneNode_quaternion_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_fov_get", BRO_NATIVE_FN(bro_scene_SceneNode_fov_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_fov_set", BRO_NATIVE_FN(bro_scene_SceneNode_fov_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_near_get", BRO_NATIVE_FN(bro_scene_SceneNode_near_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_near_set", BRO_NATIVE_FN(bro_scene_SceneNode_near_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_far_get", BRO_NATIVE_FN(bro_scene_SceneNode_far_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_far_set", BRO_NATIVE_FN(bro_scene_SceneNode_far_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_aspect_get", BRO_NATIVE_FN(bro_scene_SceneNode_aspect_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_aspect_set", BRO_NATIVE_FN(bro_scene_SceneNode_aspect_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_orthoHeight_get", BRO_NATIVE_FN(bro_scene_SceneNode_orthoHeight_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_orthoHeight_set", BRO_NATIVE_FN(bro_scene_SceneNode_orthoHeight_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_projection_get", BRO_NATIVE_FN(bro_scene_SceneNode_projection_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_projection_set", BRO_NATIVE_FN(bro_scene_SceneNode_projection_set), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneNode_worldPosition_get", BRO_NATIVE_FN(bro_scene_SceneNode_worldPosition_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_worldMatrix_get", BRO_NATIVE_FN(bro_scene_SceneNode_worldMatrix_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_parent_get", BRO_NATIVE_FN(bro_scene_SceneNode_parent_get), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_children_get", BRO_NATIVE_FN(bro_scene_SceneNode_children_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_children_get_at", BRO_NATIVE_FN(bro_scene_SceneNode_children_get_at), "__bro_native.scene.SceneNode", {"i32"}, error) &&
        fn("__bro_native.scene.SceneNode_add", BRO_NATIVE_FN(bro_scene_SceneNode_add), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_remove", BRO_NATIVE_FN(bro_scene_SceneNode_remove), "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_addChild", BRO_NATIVE_FN(bro_scene_SceneNode_addChild), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_removeChild", BRO_NATIVE_FN(bro_scene_SceneNode_removeChild), "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_destroy", BRO_NATIVE_FN(bro_scene_SceneNode_destroy), "void", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setPosition", BRO_NATIVE_FN(bro_scene_SceneNode_setPosition), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_setRotation", BRO_NATIVE_FN(bro_scene_SceneNode_setRotation), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "f64", "f64", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_setScale", BRO_NATIVE_FN(bro_scene_SceneNode_setScale), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "f64", "bool", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_lookAt", BRO_NATIVE_FN(bro_scene_SceneNode_lookAt), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_boneCount_get", BRO_NATIVE_FN(bro_scene_SceneNode_boneCount_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_skinReady_get", BRO_NATIVE_FN(bro_scene_SceneNode_skinReady_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_isPlaying_get", BRO_NATIVE_FN(bro_scene_SceneNode_isPlaying_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_currentAnimation_get", BRO_NATIVE_FN(bro_scene_SceneNode_currentAnimation_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_animationDuration_get", BRO_NATIVE_FN(bro_scene_SceneNode_animationDuration_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_animationTime_get", BRO_NATIVE_FN(bro_scene_SceneNode_animationTime_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_animationTime_set", BRO_NATIVE_FN(bro_scene_SceneNode_animationTime_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_animationSpeed_get", BRO_NATIVE_FN(bro_scene_SceneNode_animationSpeed_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_animationSpeed_set", BRO_NATIVE_FN(bro_scene_SceneNode_animationSpeed_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_state_get", BRO_NATIVE_FN(bro_scene_SceneNode_state_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setInstanceTransform", BRO_NATIVE_FN(bro_scene_SceneNode_setInstanceTransform), "void", {"__bro_native.scene.SceneNode", "i32", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_setInstanceColor", BRO_NATIVE_FN(bro_scene_SceneNode_setInstanceColor), "void", {"__bro_native.scene.SceneNode", "i32", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_setInstanceCount", BRO_NATIVE_FN(bro_scene_SceneNode_setInstanceCount), "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
        fn("__bro_native.scene.SceneNode_setHtml", BRO_NATIVE_FN(bro_scene_SceneNode_setHtml), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneNode_markHtmlDirty", BRO_NATIVE_FN(bro_scene_SceneNode_markHtmlDirty), "void", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_burst", BRO_NATIVE_FN(bro_scene_SceneNode_burst), "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
        fn("__bro_native.scene.SceneNode_clear", BRO_NATIVE_FN(bro_scene_SceneNode_clear), "void", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_probeCapture", BRO_NATIVE_FN(bro_scene_SceneNode_probeCapture), "void", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_savePly", BRO_NATIVE_FN(bro_scene_SceneNode_savePly), "bool", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneGraph_root_get", BRO_NATIVE_FN(bro_scene_SceneGraph_root_get), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraX_get", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraX_get), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraX_set", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraX_set), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraY_get", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraY_get), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraY_set", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraY_set), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraZoom_get", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraZoom_get), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraZoom_set", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraZoom_set), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_showLightIcons_get", BRO_NATIVE_FN(bro_scene_SceneGraph_showLightIcons_get), "bool", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_showLightIcons_set", BRO_NATIVE_FN(bro_scene_SceneGraph_showLightIcons_set), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_frustumCulling_get", BRO_NATIVE_FN(bro_scene_SceneGraph_frustumCulling_get), "bool", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_frustumCulling_set", BRO_NATIVE_FN(bro_scene_SceneGraph_frustumCulling_set), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_renderScale_get", BRO_NATIVE_FN(bro_scene_SceneGraph_renderScale_get), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_renderScale_set", BRO_NATIVE_FN(bro_scene_SceneGraph_renderScale_set), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_msaa_get", BRO_NATIVE_FN(bro_scene_SceneGraph_msaa_get), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_msaa_set", BRO_NATIVE_FN(bro_scene_SceneGraph_msaa_set), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_activeCamera_get", BRO_NATIVE_FN(bro_scene_SceneGraph_activeCamera_get), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_activeCamera_set", BRO_NATIVE_FN(bro_scene_SceneGraph_activeCamera_set), "void", {"__bro_native.scene.SceneGraph", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneGraph_viewMatrix_get", BRO_NATIVE_FN(bro_scene_SceneGraph_viewMatrix_get), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_projectionMatrix_get", BRO_NATIVE_FN(bro_scene_SceneGraph_projectionMatrix_get), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cameraEye_get", BRO_NATIVE_FN(bro_scene_SceneGraph_cameraEye_get), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_createNode", BRO_NATIVE_FN(bro_scene_SceneGraph_createNode), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "bool", "str", "f64[]", "f64[]", "f64[]", "bool", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_createHtmlNode", BRO_NATIVE_FN(bro_scene_SceneGraph_createHtmlNode), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "bool", "str", "bool", "f64", "bool", "f64", "f64[]", "f64[]", "f64[]", "bool", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_createLight", BRO_NATIVE_FN(bro_scene_SceneGraph_createLight), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "bool", "str", "f64[]", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "bool", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_createParticles", BRO_NATIVE_FN(bro_scene_SceneGraph_createParticles), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
        fn("__bro_native.scene.SceneGraph_createDecal", BRO_NATIVE_FN(bro_scene_SceneGraph_createDecal), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "bool", "str", "f64[]", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_createReflectionProbe", BRO_NATIVE_FN(bro_scene_SceneGraph_createReflectionProbe), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "f64[]", "bool", "i32", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_createTween", BRO_NATIVE_FN(bro_scene_SceneGraph_createTween), "__bro_native.animation.Tween", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_createAnimationPlayer", BRO_NATIVE_FN(bro_scene_SceneGraph_createAnimationPlayer), "__bro_native.animation.AnimationPlayer", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_createTerrain", BRO_NATIVE_FN(bro_scene_SceneGraph_createTerrain), "__bro_native.terrain.Terrain", {"__bro_native.scene.SceneGraph", "i32[]", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "bool", "i32", "bool", "f64", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "f64[]", "f32[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_findById", BRO_NATIVE_FN(bro_scene_SceneGraph_findById), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "i32"}, error) &&
        fn("__bro_native.scene.SceneGraph_findByName", BRO_NATIVE_FN(bro_scene_SceneGraph_findByName), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
        fn("__bro_native.scene.SceneGraph_destroyNode", BRO_NATIVE_FN(bro_scene_SceneGraph_destroyNode), "void", {"__bro_native.scene.SceneGraph", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneGraph_setCamera", BRO_NATIVE_FN(bro_scene_SceneGraph_setCamera), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64", "f64[]", "f64[]", "f64[]", "bool", "f64", "f64[]", "bool", "str", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_createCamera", BRO_NATIVE_FN(bro_scene_SceneGraph_createCamera), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64", "f64[]", "f64[]", "f64[]", "bool", "f64", "f64[]", "bool", "str", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setActiveCamera", BRO_NATIVE_FN(bro_scene_SceneGraph_setActiveCamera), "void", {"__bro_native.scene.SceneGraph", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneGraph_setToneMap", BRO_NATIVE_FN(bro_scene_SceneGraph_setToneMap), "void", {"__bro_native.scene.SceneGraph", "bool", "str", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setExposure", BRO_NATIVE_FN(bro_scene_SceneGraph_setExposure), "void", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_setAmbient", BRO_NATIVE_FN(bro_scene_SceneGraph_setAmbient), "void", {"__bro_native.scene.SceneGraph", "f64[]", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setWind", BRO_NATIVE_FN(bro_scene_SceneGraph_setWind), "void", {"__bro_native.scene.SceneGraph", "f64[]", "bool", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setWindField", BRO_NATIVE_FN(bro_scene_SceneGraph_setWindField), "void", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_windAt", BRO_NATIVE_FN(bro_scene_SceneGraph_windAt), "f64[]", {"__bro_native.scene.SceneGraph", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_windState", BRO_NATIVE_FN(bro_scene_SceneGraph_windState), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_setBody", BRO_NATIVE_FN(bro_scene_SceneGraph_setBody), "void", {"__bro_native.scene.SceneGraph", "f64", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_place", BRO_NATIVE_FN(bro_scene_SceneGraph_place), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_toBody", BRO_NATIVE_FN(bro_scene_SceneGraph_toBody), "f64[]", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_toPlace", BRO_NATIVE_FN(bro_scene_SceneGraph_toPlace), "f64[]", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_upAt", BRO_NATIVE_FN(bro_scene_SceneGraph_upAt), "f64[]", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_latLongAt", BRO_NATIVE_FN(bro_scene_SceneGraph_latLongAt), "f64[]", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_reanchorAt", BRO_NATIVE_FN(bro_scene_SceneGraph_reanchorAt), "bool", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_onReanchor_set", BRO_NATIVE_FN(bro_scene_SceneGraph_onReanchor_set), "void", {"__bro_native.scene.SceneGraph", "dynamic"}, error) &&
        fn("__bro_native.scene.SceneGraph_expect", BRO_NATIVE_FN(bro_scene_SceneGraph_expect), "f64", {"__bro_native.scene.SceneGraph", "f64[]", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_expectMove", BRO_NATIVE_FN(bro_scene_SceneGraph_expectMove), "bool", {"__bro_native.scene.SceneGraph", "f64", "f64[]", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_expectCancel", BRO_NATIVE_FN(bro_scene_SceneGraph_expectCancel), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_expectPending", BRO_NATIVE_FN(bro_scene_SceneGraph_expectPending), "f64", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setShadowQuality", BRO_NATIVE_FN(bro_scene_SceneGraph_setShadowQuality), "void", {"__bro_native.scene.SceneGraph", "bool", "i32"}, error) &&
        fn("__bro_native.scene.SceneGraph_setFog", BRO_NATIVE_FN(bro_scene_SceneGraph_setFog), "void", {"__bro_native.scene.SceneGraph", "bool", "str", "f64[]", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setAtmosphere", BRO_NATIVE_FN(bro_scene_SceneGraph_setAtmosphere), "void", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_setClouds", BRO_NATIVE_FN(bro_scene_SceneGraph_setClouds), "str", {"__bro_native.scene.SceneGraph", "str", "f64[]", "f32[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_cloudStats", BRO_NATIVE_FN(bro_scene_SceneGraph_cloudStats), "f64[]", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_setStarfield", BRO_NATIVE_FN(bro_scene_SceneGraph_setStarfield), "void", {"__bro_native.scene.SceneGraph", "bool", "i32", "bool", "f64", "bool", "f64", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_setTiltShift", BRO_NATIVE_FN(bro_scene_SceneGraph_setTiltShift), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setBloom", BRO_NATIVE_FN(bro_scene_SceneGraph_setBloom), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32"}, error) &&
        fn("__bro_native.scene.SceneGraph_setSSAO", BRO_NATIVE_FN(bro_scene_SceneGraph_setSSAO), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32"}, error) &&
        fn("__bro_native.scene.SceneGraph_setSSR", BRO_NATIVE_FN(bro_scene_SceneGraph_setSSR), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setDepthOfField", BRO_NATIVE_FN(bro_scene_SceneGraph_setDepthOfField), "void", {"__bro_native.scene.SceneGraph", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setColorLUT", BRO_NATIVE_FN(bro_scene_SceneGraph_setColorLUT), "bool", {"__bro_native.scene.SceneGraph", "str", "i32", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setFXAA", BRO_NATIVE_FN(bro_scene_SceneGraph_setFXAA), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_setGroundBounce", BRO_NATIVE_FN(bro_scene_SceneGraph_setGroundBounce), "void", {"__bro_native.scene.SceneGraph", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setProbeGrid", BRO_NATIVE_FN(bro_scene_SceneGraph_setProbeGrid), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_setDither", BRO_NATIVE_FN(bro_scene_SceneGraph_setDither), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_setFrameTarget", BRO_NATIVE_FN(bro_scene_SceneGraph_setFrameTarget), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setRenderScale", BRO_NATIVE_FN(bro_scene_SceneGraph_setRenderScale), "void", {"__bro_native.scene.SceneGraph", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_setMSAA", BRO_NATIVE_FN(bro_scene_SceneGraph_setMSAA), "void", {"__bro_native.scene.SceneGraph", "i32"}, error) &&
        fn("__bro_native.scene.SceneGraph_setEnvironment", BRO_NATIVE_FN(bro_scene_SceneGraph_setEnvironment), "bool", {"__bro_native.scene.SceneGraph", "bool", "str", "bool", "str", "f64[]", "bool", "f64", "bool", "f64", "bool", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_setFrustumCulling", BRO_NATIVE_FN(bro_scene_SceneGraph_setFrustumCulling), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_cullStats", BRO_NATIVE_FN(bro_scene_SceneGraph_cullStats), "void", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_cullStats_totalNodes", BRO_NATIVE_FN(bro_scene_SceneGraph_cullStats_totalNodes), "i32", {}, error) &&
        fn("__bro_native.scene.SceneGraph_cullStats_renderedNodes", BRO_NATIVE_FN(bro_scene_SceneGraph_cullStats_renderedNodes), "i32", {}, error) &&
        fn("__bro_native.scene.SceneGraph_cullStats_culledNodes", BRO_NATIVE_FN(bro_scene_SceneGraph_cullStats_culledNodes), "i32", {}, error) &&
        fn("__bro_native.scene.SceneGraph_clear", BRO_NATIVE_FN(bro_scene_SceneGraph_clear), "void", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_syncPhysics", BRO_NATIVE_FN(bro_scene_SceneGraph_syncPhysics), "void", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_raycast", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast), "bool", {"__bro_native.scene.SceneGraph", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneGraph_raycast_node", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast_node), "__bro_native.scene.SceneNode", {}, error) &&
        fn("__bro_native.scene.SceneGraph_raycast_point", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast_point), "f64[]", {}, error) &&
        fn("__bro_native.scene.SceneGraph_raycast_normal", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast_normal), "f64[]", {}, error) &&
        fn("__bro_native.scene.SceneGraph_raycast_distance", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast_distance), "f64", {}, error) &&
        fn("__bro_native.scene.SceneGraph_unprojectLocal", BRO_NATIVE_FN(bro_scene_SceneGraph_unprojectLocal), "f64[]", {"__bro_native.scene.SceneGraph", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_projectLocal", BRO_NATIVE_FN(bro_scene_SceneGraph_projectLocal), "f64[]", {"__bro_native.scene.SceneGraph", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneGraph_bindAudioListenerToCamera", BRO_NATIVE_FN(bro_scene_SceneGraph_bindAudioListenerToCamera), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
        fn("__bro_native.scene.SceneGraph_detachAIWorld", BRO_NATIVE_FN(bro_scene_SceneGraph_detachAIWorld), "void", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneGraph_createLand", BRO_NATIVE_FN(bro_scene_SceneGraph_createLand), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneNode_setLandModule", BRO_NATIVE_FN(bro_scene_SceneNode_setLandModule), "str", {"__bro_native.scene.SceneNode", "str", "f32[]", "str", "i32"}, error) &&
        fn("__bro_native.scene.SceneNode_setLandProcess", BRO_NATIVE_FN(bro_scene_SceneNode_setLandProcess), "str", {"__bro_native.scene.SceneNode", "str", "str", "f64[]", "f32[]"}, error) &&
        fn("__bro_native.scene.SceneNode_setLandParams", BRO_NATIVE_FN(bro_scene_SceneNode_setLandParams), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_landParams", BRO_NATIVE_FN(bro_scene_SceneNode_landParams), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_landHeight", BRO_NATIVE_FN(bro_scene_SceneNode_landHeight), "f64", {"__bro_native.scene.SceneNode", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_landStats", BRO_NATIVE_FN(bro_scene_SceneNode_landStats), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneGraph_createPlants", BRO_NATIVE_FN(bro_scene_SceneGraph_createPlants), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlantModule", BRO_NATIVE_FN(bro_scene_SceneNode_setPlantModule), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlantSpecies", BRO_NATIVE_FN(bro_scene_SceneNode_setPlantSpecies), "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlants", BRO_NATIVE_FN(bro_scene_SceneNode_setPlants), "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlantRules", BRO_NATIVE_FN(bro_scene_SceneNode_setPlantRules), "str", {"__bro_native.scene.SceneNode", "str"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlantLand", BRO_NATIVE_FN(bro_scene_SceneNode_setPlantLand), "str", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode", "bool"}, error) &&
        fn("__bro_native.scene.SceneNode_plantPlacement", BRO_NATIVE_FN(bro_scene_SceneNode_plantPlacement), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setPlantParams", BRO_NATIVE_FN(bro_scene_SceneNode_setPlantParams), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_plantStats", BRO_NATIVE_FN(bro_scene_SceneNode_plantStats), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneGraph_createGrass",BRO_NATIVE_FN(bro_scene_SceneGraph_createGrass), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph"}, error) &&
        fn("__bro_native.scene.SceneNode_setGrassGround", BRO_NATIVE_FN(bro_scene_SceneNode_setGrassGround), "i32", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setGrassParams", BRO_NATIVE_FN(bro_scene_SceneNode_setGrassParams), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
        fn("__bro_native.scene.SceneNode_grassParams", BRO_NATIVE_FN(bro_scene_SceneNode_grassParams), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setGrassMover", BRO_NATIVE_FN(bro_scene_SceneNode_setGrassMover), "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_grassPush", BRO_NATIVE_FN(bro_scene_SceneNode_grassPush), "void", {"__bro_native.scene.SceneNode", "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_grassStats", BRO_NATIVE_FN(bro_scene_SceneNode_grassStats), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
        fn("__bro_native.scene.SceneNode_setGrassHerbs", BRO_NATIVE_FN(bro_scene_SceneNode_setGrassHerbs), "void", {"__bro_native.scene.SceneNode", "str", "f32[]", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_setGrassSward", BRO_NATIVE_FN(bro_scene_SceneNode_setGrassSward), "void", {"__bro_native.scene.SceneNode", "str", "f32[]", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.scene.SceneNode_setTurfGrass", BRO_NATIVE_FN(bro_scene_SceneNode_setTurfGrass), "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneNode", "i32"}, error) &&
        fn("__bro_native.scene.SceneNode_sampleHeights", BRO_NATIVE_FN(bro_scene_SceneNode_sampleHeights), "f32[]", {"__bro_native.scene.SceneNode", "f64", "f64", "f64", "i32", "i32"}, error);
    if (!ok) return false;
    publishPrototype("scene", "__bro_native.scene.SceneNode", "SceneNodeProto");
    publishPrototype("scene", "__bro_native.scene.SceneGraph", "SceneGraphProto");
    return true;
}

bool registerSceneAttachNatives(std::string* error);
bool registerSceneShaderNatives(std::string* error);
bool registerSceneCameraNatives(std::string* error);
bool registerSceneExtraNatives(std::string* error);

bool registerSceneAttachNatives(std::string* error) {
    static const char* const G = "__bro_native.scene.SceneGraph";
    return natives::fn("__bro_native.scene.SceneGraph_attachTo", BRO_NATIVE_FN(bro_scene_SceneGraph_attachTo), "void", {"dynamic", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_detach", BRO_NATIVE_FN(bro_scene_SceneGraph_detach), "void", {G}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_attached_get", BRO_NATIVE_FN(bro_scene_SceneGraph_attached_get), "bool", {G}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_keepAlive_get", BRO_NATIVE_FN(bro_scene_SceneGraph_keepAlive_get), "bool", {G}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_keepAlive_set", BRO_NATIVE_FN(bro_scene_SceneGraph_keepAlive_set), "void", {G, "bool"}, error);
}

bool registerSceneShaderNatives(std::string* error) {
    return natives::fn("__bro_native.scene.SceneNode_setShaderTexture", BRO_NATIVE_FN(bro_scene_SceneNode_setShaderTexture), "bool", {"__bro_native.scene.SceneNode", "str", "i32", "i32", "i32", "i32", "f32[]", "bool", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setBaseColorTextureData", BRO_NATIVE_FN(bro_scene_SceneNode_setBaseColorTextureData), "void", {"__bro_native.scene.SceneNode", "i32", "i32", "u8[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_clearBaseColorTexture", BRO_NATIVE_FN(bro_scene_SceneNode_clearBaseColorTexture), "void", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setEmissionTextureData", BRO_NATIVE_FN(bro_scene_SceneNode_setEmissionTextureData), "void", {"__bro_native.scene.SceneNode", "i32", "i32", "u8[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_onAnimationEnd_set", BRO_NATIVE_FN(bro_scene_SceneNode_onAnimationEnd_set), "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_renderPriority_get", BRO_NATIVE_FN(bro_scene_SceneNode_renderPriority_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_renderPriority_set", BRO_NATIVE_FN(bro_scene_SceneNode_renderPriority_set), "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissionStrength_get", BRO_NATIVE_FN(bro_scene_SceneNode_emissionStrength_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissionStrength_set", BRO_NATIVE_FN(bro_scene_SceneNode_emissionStrength_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_upperFade_get", BRO_NATIVE_FN(bro_scene_SceneNode_upperFade_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_upperFade_set", BRO_NATIVE_FN(bro_scene_SceneNode_upperFade_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_lowerFade_get", BRO_NATIVE_FN(bro_scene_SceneNode_lowerFade_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_lowerFade_set", BRO_NATIVE_FN(bro_scene_SceneNode_lowerFade_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_normalFade_get", BRO_NATIVE_FN(bro_scene_SceneNode_normalFade_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_normalFade_set", BRO_NATIVE_FN(bro_scene_SceneNode_normalFade_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_modulate_get", BRO_NATIVE_FN(bro_scene_SceneNode_modulate_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_modulate_set", BRO_NATIVE_FN(bro_scene_SceneNode_modulate_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particleCount_get", BRO_NATIVE_FN(bro_scene_SceneNode_particleCount_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particlePlaying_get", BRO_NATIVE_FN(bro_scene_SceneNode_particlePlaying_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particleRate_get", BRO_NATIVE_FN(bro_scene_SceneNode_particleRate_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particleRate_set", BRO_NATIVE_FN(bro_scene_SceneNode_particleRate_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particleEmit", BRO_NATIVE_FN(bro_scene_SceneNode_particleEmit), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_particlePaused_get", BRO_NATIVE_FN(bro_scene_SceneNode_particlePaused_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_softness_get", BRO_NATIVE_FN(bro_scene_SceneNode_softness_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_softness_set", BRO_NATIVE_FN(bro_scene_SceneNode_softness_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_onFinished_set", BRO_NATIVE_FN(bro_scene_SceneNode_onFinished_set), "void", {"__bro_native.scene.SceneNode", "dynamic"}, error);
}

bool registerSceneCameraNatives(std::string* error) {
    return natives::fn("__bro_native.scene.SceneGraph_clearActiveCamera", BRO_NATIVE_FN(bro_scene_SceneGraph_clearActiveCamera), "void", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_raycast_instance", BRO_NATIVE_FN(bro_scene_SceneGraph_raycast_instance), "i32", {}, error);
}

bool registerSceneExtraNatives(std::string* error) {
    if (!registerSceneAttachNatives(error)) return false;
    static const char* const N = "__bro_native.scene.SceneNode";
    return natives::fn("__bro_native.scene.SceneNode_alphaCutoff_get", BRO_NATIVE_FN(bro_scene_SceneNode_alphaCutoff_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_alphaCutoff_set", BRO_NATIVE_FN(bro_scene_SceneNode_alphaCutoff_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_doubleSided_get", BRO_NATIVE_FN(bro_scene_SceneNode_doubleSided_get), "bool", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_doubleSided_set", BRO_NATIVE_FN(bro_scene_SceneNode_doubleSided_set), "void", {N, "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_interior_get", BRO_NATIVE_FN(bro_scene_SceneNode_interior_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_interior_set", BRO_NATIVE_FN(bro_scene_SceneNode_interior_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_priority_get", BRO_NATIVE_FN(bro_scene_SceneNode_priority_get), "i32", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_priority_set", BRO_NATIVE_FN(bro_scene_SceneNode_priority_set), "void", {N, "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissiveColor_get", BRO_NATIVE_FN(bro_scene_SceneNode_emissiveColor_get), "f64[]", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissiveColor_set", BRO_NATIVE_FN(bro_scene_SceneNode_emissiveColor_set), "void", {N, "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissiveColorCss_set", BRO_NATIVE_FN(bro_scene_SceneNode_emissiveColorCss_set), "void", {N, "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_nearClipDist_get", BRO_NATIVE_FN(bro_scene_SceneNode_nearClipDist_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_nearClipDist_set", BRO_NATIVE_FN(bro_scene_SceneNode_nearClipDist_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_radius_get", BRO_NATIVE_FN(bro_scene_SceneNode_radius_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_radius_set", BRO_NATIVE_FN(bro_scene_SceneNode_radius_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_fillColor_get", BRO_NATIVE_FN(bro_scene_SceneNode_fillColor_get), "str", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_fillColor_set", BRO_NATIVE_FN(bro_scene_SceneNode_fillColor_set), "void", {N, "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_strokeColor_get", BRO_NATIVE_FN(bro_scene_SceneNode_strokeColor_get), "str", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_strokeColor_set", BRO_NATIVE_FN(bro_scene_SceneNode_strokeColor_set), "void", {N, "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_strokeWidth_get", BRO_NATIVE_FN(bro_scene_SceneNode_strokeWidth_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_strokeWidth_set", BRO_NATIVE_FN(bro_scene_SceneNode_strokeWidth_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_autoSync_get", BRO_NATIVE_FN(bro_scene_SceneNode_autoSync_get), "bool", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_autoSync_set", BRO_NATIVE_FN(bro_scene_SceneNode_autoSync_set), "void", {N, "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_pixelsPerUnit_get", BRO_NATIVE_FN(bro_scene_SceneNode_pixelsPerUnit_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_pixelsPerUnit_set", BRO_NATIVE_FN(bro_scene_SceneNode_pixelsPerUnit_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_syncToPhysics", BRO_NATIVE_FN(bro_scene_SceneNode_syncToPhysics), "void", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_billboard_get", BRO_NATIVE_FN(bro_scene_SceneNode_billboard_get), "str", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_billboard_set", BRO_NATIVE_FN(bro_scene_SceneNode_billboard_set), "void", {N, "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_frameIndex_get", BRO_NATIVE_FN(bro_scene_SceneNode_frameIndex_get), "i32", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_frameIndex_set", BRO_NATIVE_FN(bro_scene_SceneNode_frameIndex_set), "void", {N, "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_updateInstance", BRO_NATIVE_FN(bro_scene_SceneNode_updateInstance), "void", {N, "i32", "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setInstancedMesh", BRO_NATIVE_FN(bro_scene_SceneNode_setInstancedMesh), "void", {N, "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setAtlasGrid", BRO_NATIVE_FN(bro_scene_SceneNode_setAtlasGrid), "void", {N, "i32", "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setScatterSegments", BRO_NATIVE_FN(bro_scene_SceneNode_setScatterSegments), "void", {N, "f32[]", "f32[]", "f64[]", "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setTubeSegments", BRO_NATIVE_FN(bro_scene_SceneNode_setTubeSegments), "void", {N, "f32[]", "i32", "f64", "f32[]", "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setVegetation", BRO_NATIVE_FN(bro_scene_SceneNode_setVegetation), "void", {N, "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_isScatter", BRO_NATIVE_FN(bro_scene_SceneNode_isScatter), "bool", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_isTube", BRO_NATIVE_FN(bro_scene_SceneNode_isTube), "bool", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_anchorX_get", BRO_NATIVE_FN(bro_scene_SceneNode_anchorX_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_anchorY_get", BRO_NATIVE_FN(bro_scene_SceneNode_anchorY_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_anchor_set", BRO_NATIVE_FN(bro_scene_SceneNode_anchor_set), "void", {N, "f64", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_cornerRadius_get", BRO_NATIVE_FN(bro_scene_SceneNode_cornerRadius_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_cornerRadius_set", BRO_NATIVE_FN(bro_scene_SceneNode_cornerRadius_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_pxPerUnit_get", BRO_NATIVE_FN(bro_scene_SceneNode_pxPerUnit_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_pxPerUnit_set", BRO_NATIVE_FN(bro_scene_SceneNode_pxPerUnit_set), "void", {N, "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_bodyId_get", BRO_NATIVE_FN(bro_scene_SceneNode_bodyId_get), "f64", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_atlasCols_get", BRO_NATIVE_FN(bro_scene_SceneNode_atlasCols_get), "i32", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_atlasRows_get", BRO_NATIVE_FN(bro_scene_SceneNode_atlasRows_get), "i32", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_staticBatch_get", BRO_NATIVE_FN(bro_scene_SceneNode_staticBatch_get), "bool", {N}, error) &&
           natives::fn("__bro_native.scene.SceneNode_staticBatch_set", BRO_NATIVE_FN(bro_scene_SceneNode_staticBatch_set), "void", {N, "bool"}, error);
}

bool registerSceneNatives(std::string* error) {
    if (!registerNatives_scene(error)) return false;

    return natives::fn("__bro_native.scene.SceneGraph_render", BRO_NATIVE_FN(bro_scene_SceneGraph_render), "void", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_canvasWidth", BRO_NATIVE_FN(bro_scene_SceneGraph_canvasWidth), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_canvasHeight", BRO_NATIVE_FN(bro_scene_SceneGraph_canvasHeight), "f64", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_setCanvasSize", BRO_NATIVE_FN(bro_scene_SceneGraph_setCanvasSize), "void", {"__bro_native.scene.SceneGraph", "f64", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_readTonemapPixels", BRO_NATIVE_FN(bro_scene_SceneGraph_readTonemapPixels), "u8[]", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_readTonemapWidth", BRO_NATIVE_FN(bro_scene_SceneGraph_readTonemapWidth), "i32", {}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_setInspect", BRO_NATIVE_FN(bro_scene_SceneGraph_setInspect), "void", {"__bro_native.scene.SceneGraph", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_inspectFrame", BRO_NATIVE_FN(bro_scene_SceneGraph_inspectFrame), "f64[]", {"__bro_native.scene.SceneGraph", "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_inspectImage", BRO_NATIVE_FN(bro_scene_SceneGraph_inspectImage), "u8[]", {}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_cloudCoverAt", BRO_NATIVE_FN(bro_scene_SceneGraph_cloudCoverAt), "f64[]", {"__bro_native.scene.SceneGraph", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_readTonemapHeight", BRO_NATIVE_FN(bro_scene_SceneGraph_readTonemapHeight), "i32", {}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createMesh", BRO_NATIVE_FN(bro_scene_SceneGraph_createMesh), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createSkinnedMesh", BRO_NATIVE_FN(bro_scene_SceneGraph_createSkinnedMesh), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createInstancedMesh", BRO_NATIVE_FN(bro_scene_SceneGraph_createInstancedMesh), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "dynamic", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createShape", BRO_NATIVE_FN(bro_scene_SceneGraph_createShape), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createSprite", BRO_NATIVE_FN(bro_scene_SceneGraph_createSprite), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createPhysicsNode", BRO_NATIVE_FN(bro_scene_SceneGraph_createPhysicsNode), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createParticles3D", BRO_NATIVE_FN(bro_scene_SceneGraph_createParticles3D), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_createGaussianSplat", BRO_NATIVE_FN(bro_scene_SceneGraph_createGaussianSplat), "__bro_native.scene.SceneNode", {"__bro_native.scene.SceneGraph", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_type_get", BRO_NATIVE_FN(bro_scene_SceneNode_type_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_kind_get", BRO_NATIVE_FN(bro_scene_SceneNode_kind_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_childCount_get", BRO_NATIVE_FN(bro_scene_SceneNode_childCount_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_castsShadow_get", BRO_NATIVE_FN(bro_scene_SceneNode_castsShadow_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_castsShadow_set", BRO_NATIVE_FN(bro_scene_SceneNode_castsShadow_set), "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_receivesShadow_get", BRO_NATIVE_FN(bro_scene_SceneNode_receivesShadow_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_receivesShadow_set", BRO_NATIVE_FN(bro_scene_SceneNode_receivesShadow_set), "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_metallic_get", BRO_NATIVE_FN(bro_scene_SceneNode_metallic_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_metallic_set", BRO_NATIVE_FN(bro_scene_SceneNode_metallic_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_roughness_get", BRO_NATIVE_FN(bro_scene_SceneNode_roughness_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_roughness_set", BRO_NATIVE_FN(bro_scene_SceneNode_roughness_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissive_get", BRO_NATIVE_FN(bro_scene_SceneNode_emissive_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_emissive_set", BRO_NATIVE_FN(bro_scene_SceneNode_emissive_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_direction_get", BRO_NATIVE_FN(bro_scene_SceneNode_direction_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_direction_set", BRO_NATIVE_FN(bro_scene_SceneNode_direction_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_color_get", BRO_NATIVE_FN(bro_scene_SceneNode_color_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_color_set", BRO_NATIVE_FN(bro_scene_SceneNode_color_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_intensity_get", BRO_NATIVE_FN(bro_scene_SceneNode_intensity_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_intensity_set", BRO_NATIVE_FN(bro_scene_SceneNode_intensity_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_range_get", BRO_NATIVE_FN(bro_scene_SceneNode_range_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_range_set", BRO_NATIVE_FN(bro_scene_SceneNode_range_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_innerAngle_get", BRO_NATIVE_FN(bro_scene_SceneNode_innerAngle_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_innerAngle_set", BRO_NATIVE_FN(bro_scene_SceneNode_innerAngle_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_outerAngle_get", BRO_NATIVE_FN(bro_scene_SceneNode_outerAngle_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_outerAngle_set", BRO_NATIVE_FN(bro_scene_SceneNode_outerAngle_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowBias_get", BRO_NATIVE_FN(bro_scene_SceneNode_shadowBias_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowBias_set", BRO_NATIVE_FN(bro_scene_SceneNode_shadowBias_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowNormalBias_get", BRO_NATIVE_FN(bro_scene_SceneNode_shadowNormalBias_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowNormalBias_set", BRO_NATIVE_FN(bro_scene_SceneNode_shadowNormalBias_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowDistance_get", BRO_NATIVE_FN(bro_scene_SceneNode_shadowDistance_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_shadowDistance_set", BRO_NATIVE_FN(bro_scene_SceneNode_shadowDistance_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setSkeleton", BRO_NATIVE_FN(bro_scene_SceneNode_setSkeleton), "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_addClip", BRO_NATIVE_FN(bro_scene_SceneNode_addClip), "void", {"__bro_native.scene.SceneNode", "str", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_addBlendSpace1D", BRO_NATIVE_FN(bro_scene_SceneNode_addBlendSpace1D), "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_addBlendSpace2D", BRO_NATIVE_FN(bro_scene_SceneNode_addBlendSpace2D), "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setBlendPos", BRO_NATIVE_FN(bro_scene_SceneNode_setBlendPos), "void", {"__bro_native.scene.SceneNode", "str", "f64", "bool", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_playLayer", BRO_NATIVE_FN(bro_scene_SceneNode_playLayer), "void", {"__bro_native.scene.SceneNode", "i32", "str", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_stopLayer", BRO_NATIVE_FN(bro_scene_SceneNode_stopLayer), "void", {"__bro_native.scene.SceneNode", "i32", "bool", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setLayerWeight", BRO_NATIVE_FN(bro_scene_SceneNode_setLayerWeight), "void", {"__bro_native.scene.SceneNode", "i32", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_addStateMachine", BRO_NATIVE_FN(bro_scene_SceneNode_addStateMachine), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_travel", BRO_NATIVE_FN(bro_scene_SceneNode_travel), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setRootMotion", BRO_NATIVE_FN(bro_scene_SceneNode_setRootMotion), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_consumeRootMotion", BRO_NATIVE_FN(bro_scene_SceneNode_consumeRootMotion), "str", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_play", BRO_NATIVE_FN(bro_scene_SceneNode_play), "void", {"__bro_native.scene.SceneNode", "str", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_stop", BRO_NATIVE_FN(bro_scene_SceneNode_stop), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_pause", BRO_NATIVE_FN(bro_scene_SceneNode_pause), "void", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_resume", BRO_NATIVE_FN(bro_scene_SceneNode_resume), "void", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setSkinningMatrices", BRO_NATIVE_FN(bro_scene_SceneNode_setSkinningMatrices), "i32", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_getBoneWorldMatrix", BRO_NATIVE_FN(bro_scene_SceneNode_getBoneWorldMatrix), "dynamic", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_blendState", BRO_NATIVE_FN(bro_scene_SceneNode_blendState), "str", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_onAnimationFinished_set", BRO_NATIVE_FN(bro_scene_SceneNode_onAnimationFinished_set), "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_onStateChanged_set", BRO_NATIVE_FN(bro_scene_SceneNode_onStateChanged_set), "void", {"__bro_native.scene.SceneNode", "dynamic"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_hasShader", BRO_NATIVE_FN(bro_scene_SceneNode_hasShader), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setShader", BRO_NATIVE_FN(bro_scene_SceneNode_setShader), "str", {"__bro_native.scene.SceneNode", "str", "str", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_clearShader", BRO_NATIVE_FN(bro_scene_SceneNode_clearShader), "void", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setShaderUniform", BRO_NATIVE_FN(bro_scene_SceneNode_setShaderUniform), "void", {"__bro_native.scene.SceneNode", "str", "f64[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setLodMeshes", BRO_NATIVE_FN(bro_scene_SceneNode_setLodMeshes), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_updateMesh", BRO_NATIVE_FN(bro_scene_SceneNode_updateMesh), "void", {"__bro_native.scene.SceneNode", "dynamic", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_lodCount", BRO_NATIVE_FN(bro_scene_SceneNode_lodCount), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_lodLevel", BRO_NATIVE_FN(bro_scene_SceneNode_lodLevel), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_visibilityRange_set", BRO_NATIVE_FN(bro_scene_SceneNode_visibilityRange_set), "void", {"__bro_native.scene.SceneNode", "f64", "f64", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_visibilityRange_clear", BRO_NATIVE_FN(bro_scene_SceneNode_visibilityRange_clear), "void", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_visibilityRange_get", BRO_NATIVE_FN(bro_scene_SceneNode_visibilityRange_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_isValid", BRO_NATIVE_FN(bro_scene_SceneGraph_isValid), "bool", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setBaseColorTextureFromScene", BRO_NATIVE_FN(bro_scene_SceneNode_setBaseColorTextureFromScene), "void", {"__bro_native.scene.SceneNode", "__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_cullStatsJson", BRO_NATIVE_FN(bro_scene_SceneGraph_cullStatsJson), "str", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneGraph_pendingWorkJson", BRO_NATIVE_FN(bro_scene_SceneGraph_pendingWorkJson), "str", {"__bro_native.scene.SceneGraph"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setInstances", BRO_NATIVE_FN(bro_scene_SceneNode_setInstances), "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setInstancesFromTransforms", BRO_NATIVE_FN(bro_scene_SceneNode_setInstancesFromTransforms), "void", {"__bro_native.scene.SceneNode", "f32[]"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_instanceCount_get", BRO_NATIVE_FN(bro_scene_SceneNode_instanceCount_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_updateMode_get", BRO_NATIVE_FN(bro_scene_SceneNode_updateMode_get), "str", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_updateMode_set", BRO_NATIVE_FN(bro_scene_SceneNode_updateMode_set), "void", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_resolution_get", BRO_NATIVE_FN(bro_scene_SceneNode_resolution_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_resolution_set", BRO_NATIVE_FN(bro_scene_SceneNode_resolution_set), "void", {"__bro_native.scene.SceneNode", "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_boxProjection_get", BRO_NATIVE_FN(bro_scene_SceneNode_boxProjection_get), "bool", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_boxProjection_set", BRO_NATIVE_FN(bro_scene_SceneNode_boxProjection_set), "void", {"__bro_native.scene.SceneNode", "bool"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_cullMargin_get", BRO_NATIVE_FN(bro_scene_SceneNode_cullMargin_get), "f64", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_cullMargin_set", BRO_NATIVE_FN(bro_scene_SceneNode_cullMargin_set), "void", {"__bro_native.scene.SceneNode", "f64"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_splatCount_get", BRO_NATIVE_FN(bro_scene_SceneNode_splatCount_get), "i32", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_setCloud", BRO_NATIVE_FN(bro_scene_SceneNode_setCloud), "void", {"__bro_native.scene.SceneNode", "f32[]", "f32[]", "f32[]", "f32[]", "f32[]", "i32"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_loadSplatPly", BRO_NATIVE_FN(bro_scene_SceneNode_loadSplatPly), "bool", {"__bro_native.scene.SceneNode", "str"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_worldAnchor_get", BRO_NATIVE_FN(bro_scene_SceneNode_worldAnchor_get), "f64[]", {"__bro_native.scene.SceneNode"}, error) &&
           natives::fn("__bro_native.scene.SceneNode_worldAnchor_set", BRO_NATIVE_FN(bro_scene_SceneNode_worldAnchor_set), "void", {"__bro_native.scene.SceneNode", "f64[]"}, error) &&
           registerSceneShaderNatives(error) &&
           registerSceneCameraNatives(error) &&
           registerSceneExtraNatives(error);
}

}  // namespace bro::bronze_host
