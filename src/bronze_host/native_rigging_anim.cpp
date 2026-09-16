#include "native_rigging_internal.h"
#include "json.hpp"

#if BRO_WITH_3D

namespace bro::bronze_host {

using json = nlohmann::json;

namespace {

// Thread-local slots for compound results
struct AutoRigSlot {
    bromesh::Skeleton skeleton;
    bromesh::SkinData skin;
    std::string methodUsed;
    std::vector<std::string> missingLandmarks;
    std::vector<std::string> warnings;
};
static thread_local AutoRigSlot tl_autoRigSlot;

static thread_local bromesh::GltfScene tl_gltfScene;

// --- Animation --------------------------------------------------------------

void animDelete(void* self) {
    delete static_cast<bromesh::Animation*>(self);
}

void* animNew(const char* jsonText) {
    auto* anim = new bromesh::Animation();
    if (jsonText && *jsonText) {
        try {
            auto j = json::parse(jsonText);
            anim->name = j.value("name", "");
            anim->duration = j.value("duration", 0.0f);
            if (j.contains("channels") && j["channels"].is_array()) {
                for (const auto& cj : j["channels"]) {
                    bromesh::AnimChannel ch;
                    ch.boneIndex = cj.value("boneIndex", -1);
                    std::string p = cj.value("path", "translation");
                    if (p == "rotation") ch.path = bromesh::AnimChannel::Path::Rotation;
                    else if (p == "scale") ch.path = bromesh::AnimChannel::Path::Scale;
                    else ch.path = bromesh::AnimChannel::Path::Translation;

                    std::string in = cj.value("interp", "linear");
                    if (in == "step") ch.interp = bromesh::AnimChannel::Interp::Step;
                    else if (in == "cubicSpline") ch.interp = bromesh::AnimChannel::Interp::CubicSpline;
                    else ch.interp = bromesh::AnimChannel::Interp::Linear;

                    if (cj.contains("times")) {
                        if (cj["times"].is_array()) {
                            for (const auto& tv : cj["times"]) ch.times.push_back(tv.get<float>());
                        } else if (cj["times"].is_object()) {
                            for (size_t i = 0; cj["times"].contains(std::to_string(i)); ++i) {
                                ch.times.push_back(cj["times"][std::to_string(i)].get<float>());
                            }
                        }
                    }
                    if (cj.contains("values")) {
                        if (cj["values"].is_array()) {
                            for (const auto& vv : cj["values"]) ch.values.push_back(vv.get<float>());
                        } else if (cj["values"].is_object()) {
                            for (size_t i = 0; cj["values"].contains(std::to_string(i)); ++i) {
                                ch.values.push_back(cj["values"][std::to_string(i)].get<float>());
                            }
                        }
                    }
                    anim->channels.push_back(std::move(ch));
                }
            }
        } catch (...) {}
    }
    return anim;
}

const char* animNameGet(void* self) {
    return natives::strResult(A(self).name);
}

void animNameSet(void* self, const char* name) {
    A(self).name = name ? name : "";
}

double animDurationGet(void* self) {
    return static_cast<double>(A(self).duration);
}

void animDurationSet(void* self, double d) {
    A(self).duration = static_cast<float>(d);
}

double animChannelCount(void* self) {
    return static_cast<double>(A(self).channels.size());
}

const char* animChannelsJSON(void* self) {
    json arr = json::array();
    for (const auto& ch : A(self).channels) {
        json cj;
        cj["boneIndex"] = ch.boneIndex;
        cj["path"] = (ch.path == bromesh::AnimChannel::Path::Rotation ? "rotation" :
                      ch.path == bromesh::AnimChannel::Path::Scale ? "scale" : "translation");
        cj["interp"] = (ch.interp == bromesh::AnimChannel::Interp::Step ? "step" :
                        ch.interp == bromesh::AnimChannel::Interp::CubicSpline ? "cubicSpline" : "linear");
        cj["times"] = ch.times;
        cj["values"] = ch.values;
        arr.push_back(cj);
    }
    return natives::strResult(arr.dump());
}

void* animEvaluate(void* self, void* skeleton, double t, bool loop) {
    return new bromesh::Pose(bromesh::evaluateAnimation(K(skeleton), A(self), static_cast<float>(t), loop));
}

void animEvaluateInto(void* self, void* skeleton, double t, bool loop, void* pose) {
    bromesh::evaluateAnimationInto(K(skeleton), A(self), static_cast<float>(t), loop, P(pose));
}

void* animRetarget(void* self, void* srcSkel, void* dstSkel) {
    return new bromesh::Animation(bromesh::retargetAnimation(A(self), K(srcSkel), K(dstSkel)));
}

// --- IK ---------------------------------------------------------------------

bool ikTwoBone(void* skel, void* pose, int32_t root, int32_t mid, int32_t end,
               const double* target, uint32_t target_len,
               const double* pole, uint32_t pole_len) {
    if (!target || target_len < 3) return false;
    float tgt[3] = { static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2]) };
    float pol[3];
    const float* polPtr = nullptr;
    if (pole && pole_len >= 3) {
        pol[0] = static_cast<float>(pole[0]);
        pol[1] = static_cast<float>(pole[1]);
        pol[2] = static_cast<float>(pole[2]);
        polPtr = pol;
    }
    return bromesh::solveTwoBoneIK(K(skel), P(pose), root, mid, end, tgt, polPtr);
}

bool ikFABRIK(void* skel, void* pose, const int32_t* chain, uint32_t chain_len,
              const double* target, uint32_t target_len, int32_t iters, double tol) {
    if (!target || target_len < 3) return false;
    float tgt[3] = { static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2]) };
    std::vector<int> ch(chain, chain + chain_len);
    return bromesh::solveFABRIK(K(skel), P(pose), ch, tgt, iters > 0 ? iters : 10,
                                tol > 0 ? static_cast<float>(tol) : 1e-3f);
}

bool ikLookAt(void* skel, void* pose, int32_t bone,
              const double* target, uint32_t target_len,
              const double* fwd, uint32_t fwd_len,
              const double* up, uint32_t up_len) {
    if (!target || target_len < 3) return false;
    float tgt[3] = { static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2]) };
    float fwdVec[3], upVec[3];
    const float* fwdPtr = nullptr;
    const float* upPtr = nullptr;
    if (fwd && fwd_len >= 3) {
        fwdVec[0] = static_cast<float>(fwd[0]);
        fwdVec[1] = static_cast<float>(fwd[1]);
        fwdVec[2] = static_cast<float>(fwd[2]);
        fwdPtr = fwdVec;
    }
    if (up && up_len >= 3) {
        upVec[0] = static_cast<float>(up[0]);
        upVec[1] = static_cast<float>(up[1]);
        upVec[2] = static_cast<float>(up[2]);
        upPtr = upVec;
    }
    return bromesh::solveLookAt(K(skel), P(pose), bone, tgt, fwdPtr, upPtr);
}

// --- RigSpec ----------------------------------------------------------------

void rigSpecDelete(void* self) {
    delete static_cast<bromesh::RigSpec*>(self);
}

void* rigSpecNew(const char* name) {
    return new bromesh::RigSpec(name && *name ? bromesh::builtinRigSpec(name) : bromesh::RigSpec{});
}

const char* rigSpecName(void* self) {
    return natives::strResult(R(self).name);
}

double rigSpecBoneCount(void* self) {
    return static_cast<double>(R(self).bones.size());
}

double rigSpecLandmarkCount(void* self) {
    return static_cast<double>(R(self).landmarks.size());
}

const char* rigSpecToJSON(void* self) {
    return natives::strResult(bromesh::serializeRigSpecJSON(R(self)));
}

const char* rigSpecLandmarkNames(void* self) {
    json arr = json::array();
    for (const auto& lm : R(self).landmarks) arr.push_back(lm.name);
    return natives::strResult(arr.dump());
}

const char* rigSpecBoneNames(void* self) {
    json arr = json::array();
    for (const auto& b : R(self).bones) arr.push_back(b.name);
    return natives::strResult(arr.dump());
}

// --- Rig --------------------------------------------------------------------

void* rigSpecByName(const char* name) {
    return new bromesh::RigSpec(bromesh::builtinRigSpec(name ? name : ""));
}

void* rigSpecFromJSON(const char* jsonText) {
    return new bromesh::RigSpec(bromesh::parseRigSpecJSON(jsonText ? jsonText : ""));
}

void* rigSpecFromFile(const char* path) {
    return new bromesh::RigSpec(bromesh::loadRigSpecFile(path ? path : ""));
}

const char* rigDetectHumanoid(void* mesh) {
    auto lm = bromesh::detectHumanoidLandmarks(M(mesh));
    json out = json::object();
    for (const auto& [name, pt] : lm.points) {
        out[name] = { pt[0], pt[1], pt[2] };
    }
    return natives::strResult(out.dump());
}

const char* rigDetectQuadruped(void* mesh) {
    auto lm = bromesh::detectQuadrupedLandmarks(M(mesh));
    json out = json::object();
    for (const auto& [name, pt] : lm.points) {
        out[name] = { pt[0], pt[1], pt[2] };
    }
    return natives::strResult(out.dump());
}

bromesh::Landmarks parseLandmarksJSON(const char* jsonText) {
    bromesh::Landmarks lm;
    if (!jsonText || !*jsonText) return lm;
    try {
        auto j = json::parse(jsonText);
        if (j.is_object()) {
            for (auto& [key, val] : j.items()) {
                if (val.is_array() && val.size() >= 3) {
                    lm.set(key, val[0].get<float>(), val[1].get<float>(), val[2].get<float>());
                }
            }
        }
    } catch (...) {}
    return lm;
}

const char* rigMissingLandmarks(void* spec, const char* landmarksJson) {
    auto lm = parseLandmarksJSON(landmarksJson);
    auto missing = bromesh::missingLandmarks(R(spec), lm);
    json arr = missing;
    return natives::strResult(arr.dump());
}

void* rigFitSkeleton(void* spec, const char* landmarksJson, void* mesh) {
    auto lm = parseLandmarksJSON(landmarksJson);
    auto skel = bromesh::fitSkeleton(R(spec), lm, M(mesh));
    return new bromesh::Skeleton(std::move(skel));
}

void rigAutoRig(void* mesh, void* spec, const char* landmarksJson, const char* optsJson) {
    auto lm = parseLandmarksJSON(landmarksJson);
    bromesh::WeightingOptions wopts;
    if (optsJson && *optsJson) {
        try {
            auto j = json::parse(optsJson);
            std::string methodStr = j.value("method", "auto");
            wopts.method = bromesh::parseWeightingMethod(methodStr.c_str());

            if (j.contains("smoothIterations")) wopts.smoothIterations = j["smoothIterations"].get<int>();
            if (j.contains("voxelResolution")) wopts.voxel.maxResolution = j["voxelResolution"].get<int>();
            if (j.contains("maxResolution")) wopts.voxel.maxResolution = j["maxResolution"].get<int>();
        } catch (...) {}
    }

    auto res = bromesh::autoRig(M(mesh), R(spec), lm, wopts);
    tl_autoRigSlot.skeleton = std::move(res.skeleton);
    tl_autoRigSlot.skin = std::move(res.skin);
    tl_autoRigSlot.missingLandmarks = std::move(res.missingLandmarks);
    tl_autoRigSlot.warnings = std::move(res.warnings);
    tl_autoRigSlot.methodUsed = bromesh::weightingMethodName(res.methodUsed);
}

void* rigAutoRigSkeleton() {
    return new bromesh::Skeleton(std::move(tl_autoRigSlot.skeleton));
}

void* rigAutoRigSkin() {
    return new bromesh::SkinData(std::move(tl_autoRigSlot.skin));
}

const char* rigAutoRigMethodUsed() {
    return natives::strResult(tl_autoRigSlot.methodUsed);
}

const char* rigAutoRigMissingLandmarks() {
    json arr = tl_autoRigSlot.missingLandmarks;
    return natives::strResult(arr.dump());
}

const char* rigAutoRigWarnings() {
    json arr = tl_autoRigSlot.warnings;
    return natives::strResult(arr.dump());
}

void* rigGenerateLocomotionCycle(void* skel, void* spec, const char* optsJson) {
    bromesh::LocomotionParams params;
    if (optsJson && *optsJson) {
        try {
            auto j = json::parse(optsJson);
            if (j.contains("strideLength")) params.strideLength = j["strideLength"].get<float>();
            if (j.contains("cycleDuration")) params.cycleDuration = j["cycleDuration"].get<float>();
            if (j.contains("footLiftHeight")) params.footLiftHeight = j["footLiftHeight"].get<float>();
            if (j.contains("keyframesPerCycle")) params.keyframesPerCycle = j["keyframesPerCycle"].get<int>();
            if (j.contains("bodyBobAmplitude")) params.bodyBobAmplitude = j["bodyBobAmplitude"].get<float>();
            if (j.contains("armSwingAmplitude")) params.armSwingAmplitude = j["armSwingAmplitude"].get<float>();
        } catch (...) {}
    }
    auto anim = bromesh::generateLocomotionCycle(K(skel), R(spec), params);
    return new bromesh::Animation(std::move(anim));
}

void* rigTransferWeights(void* targetMesh, void* sourceMesh, void* sourceSkin) {
    auto out = bromesh::transferSkinWeights(M(targetMesh), M(sourceMesh), S(sourceSkin));
    return new bromesh::SkinData(std::move(out));
}

// --- Mesh Rigging Extensions -----------------------------------------------

void meshApplySkinning(void* mesh, void* skin, const float* poseMatrices, uint32_t matrices_len) {
    if (mesh && skin && poseMatrices && matrices_len >= 16) {
        bromesh::applySkinning(M(mesh), S(skin), poseMatrices);
    }
}

void meshApplyMorphTarget(void* mesh, const char* name,
                          const float* dp, uint32_t dp_len,
                          const float* dn, uint32_t dn_len,
                          double weight) {
    if (!mesh) return;
    bromesh::MorphTarget mt;
    mt.name = name ? name : "";
    if (dp && dp_len > 0) mt.deltaPositions.assign(dp, dp + dp_len);
    if (dn && dn_len > 0) mt.deltaNormals.assign(dn, dn + dn_len);
    bromesh::applyMorphTarget(M(mesh), mt, static_cast<float>(weight));
}

bool meshSaveGLTFRigged(void* mesh, Value skinVal, Value skelVal, const char* animsJson, const char* path) {
    if (!mesh || !path) return false;
    std::vector<bromesh::Animation> anims;
    if (animsJson && *animsJson) {
        try {
            auto j = json::parse(animsJson);
            for (const auto& aj : j) {
                bromesh::Animation a;
                a.name = aj.value("name", "");
                a.duration = aj.value("duration", 0.0f);
                if (aj.contains("channels") && aj["channels"].is_array()) {
                    for (const auto& cj : aj["channels"]) {
                        bromesh::AnimChannel ch;
                        ch.boneIndex = cj.value("boneIndex", -1);
                        std::string p = cj.value("path", "translation");
                        if (p == "rotation") ch.path = bromesh::AnimChannel::Path::Rotation;
                        else if (p == "scale") ch.path = bromesh::AnimChannel::Path::Scale;
                        else ch.path = bromesh::AnimChannel::Path::Translation;

                        std::string in = cj.value("interp", "linear");
                        if (in == "step") ch.interp = bromesh::AnimChannel::Interp::Step;
                        else if (in == "cubicSpline") ch.interp = bromesh::AnimChannel::Interp::CubicSpline;
                        else ch.interp = bromesh::AnimChannel::Interp::Linear;

                        if (cj.contains("times")) {
                            if (cj["times"].is_array()) {
                                for (const auto& tv : cj["times"]) ch.times.push_back(tv.get<float>());
                            } else if (cj["times"].is_object()) {
                                for (size_t i = 0; cj["times"].contains(std::to_string(i)); ++i) {
                                    ch.times.push_back(cj["times"][std::to_string(i)].get<float>());
                                }
                            }
                        }
                        if (cj.contains("values")) {
                            if (cj["values"].is_array()) {
                                for (const auto& vv : cj["values"]) ch.values.push_back(vv.get<float>());
                            } else if (cj["values"].is_object()) {
                                for (size_t i = 0; cj["values"].contains(std::to_string(i)); ++i) {
                                    ch.values.push_back(cj["values"][std::to_string(i)].get<float>());
                                }
                            }
                        }
                        a.channels.push_back(std::move(ch));
                    }
                }
                anims.push_back(std::move(a));
            }
        } catch (...) {}
    }
    const bromesh::SkinData* sd = ev::isObject(skinVal) ? static_cast<const bromesh::SkinData*>(ev::handleData(skinVal)) : nullptr;
    const bromesh::Skeleton* sk = ev::isObject(skelVal) ? static_cast<const bromesh::Skeleton*>(ev::handleData(skelVal)) : nullptr;
    return bromesh::saveGLTF(M(mesh), sd, sk, anims, path);
}

bool meshLoadGLTF(const char* path) {
    if (!path || !*path) return false;
    tl_gltfScene = bromesh::loadGLTF(path);
    return !tl_gltfScene.meshes.empty() || !tl_gltfScene.skeletons.empty();
}

double meshLoadGLTFMeshCount() {
    return static_cast<double>(tl_gltfScene.meshes.size());
}

void* meshLoadGLTFTakeMesh(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= tl_gltfScene.meshes.size()) return nullptr;
    return new bromesh::MeshData(std::move(tl_gltfScene.meshes[idx]));
}

double meshLoadGLTFSkinCount() {
    return static_cast<double>(tl_gltfScene.skins.size());
}

void* meshLoadGLTFTakeSkin(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= tl_gltfScene.skins.size()) return nullptr;
    return new bromesh::SkinData(std::move(tl_gltfScene.skins[idx]));
}

double meshLoadGLTFSkeletonCount() {
    return static_cast<double>(tl_gltfScene.skeletons.size());
}

void* meshLoadGLTFTakeSkeleton(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= tl_gltfScene.skeletons.size()) return nullptr;
    return new bromesh::Skeleton(std::move(tl_gltfScene.skeletons[idx]));
}

double meshLoadGLTFAnimationCount() {
    return static_cast<double>(tl_gltfScene.animations.size());
}

void* meshLoadGLTFTakeAnimation(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= tl_gltfScene.animations.size()) return nullptr;
    return new bromesh::Animation(std::move(tl_gltfScene.animations[idx]));
}

void meshLoadGLTFMeshSkeleton(bronze_native_buffer* out) {
    copyOut(tl_gltfScene.meshSkeleton, out);
}

void meshLoadGLTFAnimationSkeleton(bronze_native_buffer* out) {
    copyOut(tl_gltfScene.animationSkeleton, out);
}

}  // namespace

bool registerRiggingAnimNatives(std::string* error) {
    const bool ok =
        // Animation
        ctor(kAnimation, p(&animNew), &animDelete, bronze::runtime::Finalize::InSweep, {"str"}, error) &&
        fn("__bro_native.rigging.Animation_name_get", p(&animNameGet), "str", {kAnimation}, error) &&
        fn("__bro_native.rigging.Animation_name_set", p(&animNameSet), "void", {kAnimation, "str"}, error) &&
        fn("__bro_native.rigging.Animation_duration_get", p(&animDurationGet), "f64", {kAnimation}, error) &&
        fn("__bro_native.rigging.Animation_duration_set", p(&animDurationSet), "void", {kAnimation, "f64"}, error) &&
        fn("__bro_native.rigging.Animation_channelCount_get", p(&animChannelCount), "f64", {kAnimation}, error) &&
        fn("__bro_native.rigging.Animation_channelsJSON", p(&animChannelsJSON), "str", {kAnimation}, error) &&
        fn("__bro_native.rigging.Animation_evaluate", p(&animEvaluate), kPose, {kAnimation, kSkeleton, "f64", "bool"}, error) &&
        fn("__bro_native.rigging.Animation_evaluateInto", p(&animEvaluateInto), "void", {kAnimation, kSkeleton, "f64", "bool", kPose}, error) &&
        fn("__bro_native.rigging.Animation_retarget", p(&animRetarget), kAnimation, {kAnimation, kSkeleton, kSkeleton}, error) &&

        // IK
        fn("__bro_native.rigging.IK_twoBone", p(&ikTwoBone), "bool", {kSkeleton, kPose, "i32", "i32", "i32", "f64[]", "f64[]"}, error) &&
        fn("__bro_native.rigging.IK_FABRIK", p(&ikFABRIK), "bool", {kSkeleton, kPose, "i32[]", "f64[]", "i32", "f64"}, error) &&
        fn("__bro_native.rigging.IK_lookAt", p(&ikLookAt), "bool", {kSkeleton, kPose, "i32", "f64[]", "f64[]", "f64[]"}, error) &&

        // RigSpec
        ctor(kRigSpec, p(&rigSpecNew), &rigSpecDelete, bronze::runtime::Finalize::InSweep, {"str"}, error) &&
        fn("__bro_native.rigging.RigSpec_name_get", p(&rigSpecName), "str", {kRigSpec}, error) &&
        fn("__bro_native.rigging.RigSpec_boneCount_get", p(&rigSpecBoneCount), "f64", {kRigSpec}, error) &&
        fn("__bro_native.rigging.RigSpec_landmarkCount_get", p(&rigSpecLandmarkCount), "f64", {kRigSpec}, error) &&
        fn("__bro_native.rigging.RigSpec_toJSON", p(&rigSpecToJSON), "str", {kRigSpec}, error) &&
        fn("__bro_native.rigging.RigSpec_landmarkNames", p(&rigSpecLandmarkNames), "str", {kRigSpec}, error) &&
        fn("__bro_native.rigging.RigSpec_boneNames", p(&rigSpecBoneNames), "str", {kRigSpec}, error) &&

        // Rig
        fn("__bro_native.rigging.Rig_spec", p(&rigSpecByName), kRigSpec, {"str"}, error) &&
        fn("__bro_native.rigging.Rig_specFromJSON", p(&rigSpecFromJSON), kRigSpec, {"str"}, error) &&
        fn("__bro_native.rigging.Rig_specFromFile", p(&rigSpecFromFile), kRigSpec, {"str"}, error) &&
        fn("__bro_native.rigging.Rig_detectHumanoid", p(&rigDetectHumanoid), "str", {kMesh}, error) &&
        fn("__bro_native.rigging.Rig_detectQuadruped", p(&rigDetectQuadruped), "str", {kMesh}, error) &&
        fn("__bro_native.rigging.Rig_missingLandmarks", p(&rigMissingLandmarks), "str", {kRigSpec, "str"}, error) &&
        fn("__bro_native.rigging.Rig_fitSkeleton", p(&rigFitSkeleton), kSkeleton, {kRigSpec, "str", kMesh}, error) &&
        fn("__bro_native.rigging.Rig_autoRig", p(&rigAutoRig), "void", {kMesh, kRigSpec, "str", "str"}, error) &&
        fn("__bro_native.rigging.Rig_autoRig_skeleton", p(&rigAutoRigSkeleton), kSkeleton, {}, error) &&
        fn("__bro_native.rigging.Rig_autoRig_skin", p(&rigAutoRigSkin), kSkinData, {}, error) &&
        fn("__bro_native.rigging.Rig_autoRig_methodUsed", p(&rigAutoRigMethodUsed), "str", {}, error) &&
        fn("__bro_native.rigging.Rig_autoRig_missingLandmarks", p(&rigAutoRigMissingLandmarks), "str", {}, error) &&
        fn("__bro_native.rigging.Rig_autoRig_warnings", p(&rigAutoRigWarnings), "str", {}, error) &&
        fn("__bro_native.rigging.Rig_generateLocomotionCycle", p(&rigGenerateLocomotionCycle), kAnimation, {kSkeleton, kRigSpec, "str"}, error) &&
        fn("__bro_native.rigging.Rig_transferWeights", p(&rigTransferWeights), kSkinData, {kMesh, kMesh, kSkinData}, error) &&

        // Mesh Rigging Extensions
        fn("__bro_native.rigging.Mesh_applySkinning", p(&meshApplySkinning), "void", {kMesh, kSkinData, "f32[]"}, error) &&
        fn("__bro_native.rigging.Mesh_applyMorphTarget", p(&meshApplyMorphTarget), "void", {kMesh, "str", "f32[]", "f32[]", "f64"}, error) &&
        fn("__bro_native.rigging.Mesh_saveGLTFRigged", p(&meshSaveGLTFRigged), "bool", {kMesh, "dynamic", "dynamic", "str", "str"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF", p(&meshLoadGLTF), "bool", {"str"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_meshCount", p(&meshLoadGLTFMeshCount), "f64", {}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_takeMesh", p(&meshLoadGLTFTakeMesh), kMesh, {"i32"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_skinCount", p(&meshLoadGLTFSkinCount), "f64", {}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_takeSkin", p(&meshLoadGLTFTakeSkin), kSkinData, {"i32"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_skeletonCount", p(&meshLoadGLTFSkeletonCount), "f64", {}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_takeSkeleton", p(&meshLoadGLTFTakeSkeleton), kSkeleton, {"i32"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_animationCount", p(&meshLoadGLTFAnimationCount), "f64", {}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_takeAnimation", p(&meshLoadGLTFTakeAnimation), kAnimation, {"i32"}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_meshSkeleton", p(&meshLoadGLTFMeshSkeleton), "i32[]", {}, error) &&
        fn("__bro_native.rigging.Mesh_loadGLTF_animationSkeleton", p(&meshLoadGLTFAnimationSkeleton), "i32[]", {}, error);

    return ok;
}

bool registerRiggingNatives(std::string* error) {
    return registerRiggingCoreNatives(error) && registerRiggingAnimNatives(error);
}

void publishRiggingPrototypes(bronze::Value nativeRoot) {
    ev::Persistent root(nativeRoot);
    ev::Persistent ns(ev::getProperty(root.get(), "rigging"));
    if (!ev::isObject(ns.get())) return;

    auto pub = [&](const char* className, const char* protoName) {
        ev::GlobalValue g = ev::nativeClassPrototype(className);
        if (g.found) {
            ev::Persistent p(g.value);
            ev::setProperty(ns.get(), protoName, p.get());
        }
    };
    pub(kSkinData, "SkinDataProto");
    pub(kSkeleton, "SkeletonProto");
    pub(kPose, "PoseProto");
    pub(kAnimation, "AnimationProto");
    pub(kRigSpec, "RigSpecProto");
    pub(kVoxelChunk, "VoxelChunkProto");
}

}  // namespace bro::bronze_host

#else  // !BRO_WITH_3D

namespace bro::bronze_host {
bool registerRiggingNatives(std::string*) { return true; }
void publishRiggingPrototypes(bronze::Value) {}
}

#endif
