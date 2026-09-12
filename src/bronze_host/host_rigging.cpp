#if BRO_WITH_3D

#include "bronze_host/host_rigging_internal.h"
#include "bronze_host/host_mesh_internal.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

HostClass g_skinDataClass;
HostClass g_skeletonClass;
HostClass g_poseClass;
HostClass g_animationClass;
HostClass g_rigSpecClass;
HostClass g_voxelChunkClass;

static void hostSkinDataDtor(void* p) {
    delete static_cast<HostSkinData*>(p);
}

static void hostSkeletonDtor(void* p) {
    delete static_cast<HostSkeleton*>(p);
}

static void hostPoseDtor(void* p) {
    delete static_cast<HostPose*>(p);
}

static void hostRigSpecDtor(void* p) {
    delete static_cast<HostRigSpec*>(p);
}

static void hostVoxelChunkDtor(void* p) {
    delete static_cast<HostVoxelChunk*>(p);
}

Value wrapSkinData(std::unique_ptr<bromesh::SkinData> data) {
    if (!data) return ev::undefined();
    auto* cell = new HostSkinData();
    cell->data = std::move(data);
    return g_skinDataClass.make(cell, hostSkinDataDtor);
}

Value wrapSkinData(bromesh::SkinData&& data) {
    auto* cell = new HostSkinData();
    cell->data = std::make_unique<bromesh::SkinData>(std::move(data));
    return g_skinDataClass.make(cell, hostSkinDataDtor);
}

Value wrapSkeleton(std::unique_ptr<bromesh::Skeleton> skel) {
    if (!skel) return ev::undefined();
    auto* cell = new HostSkeleton();
    cell->skel = std::move(skel);
    return g_skeletonClass.make(cell, hostSkeletonDtor);
}

Value wrapSkeleton(bromesh::Skeleton&& skel) {
    auto* cell = new HostSkeleton();
    cell->skel = std::make_unique<bromesh::Skeleton>(std::move(skel));
    return g_skeletonClass.make(cell, hostSkeletonDtor);
}

Value wrapPose(std::unique_ptr<bromesh::Pose> pose) {
    if (!pose) return ev::undefined();
    auto* cell = new HostPose();
    cell->pose = std::move(pose);
    return g_poseClass.make(cell, hostPoseDtor);
}

Value wrapPose(bromesh::Pose&& pose) {
    auto* cell = new HostPose();
    cell->pose = std::make_unique<bromesh::Pose>(std::move(pose));
    return g_poseClass.make(cell, hostPoseDtor);
}

Value wrapRigSpec(std::unique_ptr<bromesh::RigSpec> spec) {
    if (!spec) return ev::undefined();
    auto* cell = new HostRigSpec();
    cell->spec = std::move(spec);
    return g_rigSpecClass.make(cell, hostRigSpecDtor);
}

Value wrapRigSpec(bromesh::RigSpec&& spec) {
    auto* cell = new HostRigSpec();
    cell->spec = std::make_unique<bromesh::RigSpec>(std::move(spec));
    return g_rigSpecClass.make(cell, hostRigSpecDtor);
}

Value wrapVoxelChunk(std::unique_ptr<bromesh::VoxelChunk> chunk) {
    if (!chunk) return ev::undefined();
    auto* cell = new HostVoxelChunk();
    cell->chunk = std::move(chunk);
    return g_voxelChunkClass.make(cell, hostVoxelChunkDtor);
}

namespace {

void decorateSkinDataProto(ObjectBuilder& b) {
    b.accessor("boneWeights",
        [](Value self, std::span<const Value>) {
            auto* sd = hostSkinDataOf(self);
            return sd ? makeFloatArray(sd->boneWeights) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* sd = hostSkinDataOf(self);
            if (sd && !a.empty()) readFloatVector(a[0], sd->boneWeights);
            return ev::undefined();
        });
    b.accessor("boneIndices",
        [](Value self, std::span<const Value>) {
            auto* sd = hostSkinDataOf(self);
            return sd ? makeUint32Array(sd->boneIndices) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* sd = hostSkinDataOf(self);
            if (sd && !a.empty()) readU32Vector(a[0], sd->boneIndices);
            return ev::undefined();
        });
    b.accessor("inverseBindMatrices",
        [](Value self, std::span<const Value>) {
            auto* sd = hostSkinDataOf(self);
            return sd ? makeFloatArray(sd->inverseBindMatrices) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* sd = hostSkinDataOf(self);
            if (sd && !a.empty()) readFloatVector(a[0], sd->inverseBindMatrices);
            return ev::undefined();
        });
    b.accessor("boneCount",
        [](Value self, std::span<const Value>) {
            auto* sd = hostSkinDataOf(self);
            return sd ? ev::fromDouble(static_cast<double>(sd->boneCount)) : ev::fromDouble(0.0);
        },
        [](Value self, std::span<const Value> a) {
            auto* sd = hostSkinDataOf(self);
            if (sd && !a.empty() && ev::isNumber(a[0])) {
                double n = ev::toDouble(a[0]);
                sd->boneCount = static_cast<size_t>(n < 0 ? 0 : n);
            }
            return ev::undefined();
        });
    b.accessor("vertexCount",
        [](Value self, std::span<const Value>) {
            auto* sd = hostSkinDataOf(self);
            return sd ? ev::fromDouble(static_cast<double>(sd->boneWeights.size() / 4)) : ev::fromDouble(0.0);
        }, nullptr);
    b.def("clone", 0, [](Value self, std::span<const Value>) {
        auto* sd = hostSkinDataOf(self);
        if (!sd) return ev::undefined();
        return wrapSkinData(bromesh::SkinData(*sd));
    });
    b.def("normalize", 0, [](Value self, std::span<const Value>) {
        auto* sd = hostSkinDataOf(self);
        if (sd) bromesh::normalizeWeights(*sd);
        return self;
    });
}

void decorateSkeletonProto(ObjectBuilder& b) {
    b.accessor("bones",
        [](Value self, std::span<const Value>) {
            auto* sk = hostSkeletonOf(self);
            return sk ? makeBonesArray(sk->bones) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* sk = hostSkeletonOf(self);
            if (sk && !a.empty()) readBonesArray(a[0], sk->bones);
            return ev::undefined();
        });
    b.accessor("sockets",
        [](Value self, std::span<const Value>) {
            auto* sk = hostSkeletonOf(self);
            return sk ? makeSocketsArray(sk->sockets) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* sk = hostSkeletonOf(self);
            if (sk && !a.empty()) readSocketsArray(a[0], sk->sockets);
            return ev::undefined();
        });
    b.accessor("boneCount",
        [](Value self, std::span<const Value>) {
            auto* sk = hostSkeletonOf(self);
            return sk ? ev::fromDouble(static_cast<double>(sk->bones.size())) : ev::fromDouble(0.0);
        }, nullptr);
    b.accessor("socketCount",
        [](Value self, std::span<const Value>) {
            auto* sk = hostSkeletonOf(self);
            return sk ? ev::fromDouble(static_cast<double>(sk->sockets.size())) : ev::fromDouble(0.0);
        }, nullptr);
    b.def("findBone", 1, [](Value self, std::span<const Value> a) {
        auto* sk = hostSkeletonOf(self);
        if (!sk || a.empty() || !ev::isString(a[0])) return ev::fromDouble(-1.0);
        return ev::fromDouble(sk->findBone(ev::toUtf8(a[0])));
    });
    b.def("findSocket", 1, [](Value self, std::span<const Value> a) {
        auto* sk = hostSkeletonOf(self);
        if (!sk || a.empty() || !ev::isString(a[0])) return ev::fromDouble(-1.0);
        return ev::fromDouble(sk->findSocket(ev::toUtf8(a[0])));
    });
    b.def("addSocket", 1, [](Value self, std::span<const Value> a) {
        auto* sk = hostSkeletonOf(self);
        if (!sk || a.empty() || !ev::isObject(a[0])) return ev::fromDouble(-1.0);
        sk->sockets.push_back(readSocket(a[0]));
        return ev::fromDouble(static_cast<double>(sk->sockets.size() - 1));
    });
    b.def("bindPose", 0, [](Value self, std::span<const Value>) {
        auto* sk = hostSkeletonOf(self);
        if (!sk) return ev::undefined();
        return wrapPose(bromesh::bindPose(*sk));
    });
    b.def("addRigifySockets", 0, [](Value self, std::span<const Value>) {
        auto* sk = hostSkeletonOf(self);
        return ev::fromDouble(sk ? static_cast<double>(bromesh::addRigifySockets(*sk)) : 0.0);
    });
    b.def("findBoneBySuffix", 1, [](Value self, std::span<const Value> a) {
        auto* sk = hostSkeletonOf(self);
        if (!sk || a.empty() || !ev::isString(a[0])) return ev::fromDouble(-1.0);
        return ev::fromDouble(bromesh::findBoneBySuffix(*sk, ev::toUtf8(a[0])));
    });
}

void decoratePoseProto(ObjectBuilder& b) {
    b.accessor("data",
        [](Value self, std::span<const Value>) {
            auto* pw = hostPoseOf(self);
            return pw ? makeFloatArray(pw->data) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) {
            auto* pw = hostPoseOf(self);
            if (pw && !a.empty()) readFloatVector(a[0], pw->data);
            return ev::undefined();
        });
    b.accessor("boneCount",
        [](Value self, std::span<const Value>) {
            auto* pw = hostPoseOf(self);
            return pw ? ev::fromDouble(static_cast<double>(pw->boneCount())) : ev::fromDouble(0.0);
        }, nullptr);
    b.def("clone", 0, [](Value self, std::span<const Value>) {
        auto* pw = hostPoseOf(self);
        if (!pw) return ev::undefined();
        return wrapPose(bromesh::Pose(*pw));
    });
    b.def("computeWorldMatrices", 1, [](Value self, std::span<const Value> a) {
        auto* pw = hostPoseOf(self);
        if (!pw) return ev::undefined();
        if (a.empty()) return ev::throwTypeError("computeWorldMatrices requires a Skeleton");
        auto* sk = hostSkeletonOf(a[0]);
        if (!sk) return ev::throwTypeError("argument must be a Skeleton");
        std::vector<float> out;
        bromesh::computeWorldMatrices(*sk, *pw, out);
        return makeFloatArray(out);
    });
    b.def("computeSkinningMatrices", 1, [](Value self, std::span<const Value> a) {
        auto* pw = hostPoseOf(self);
        if (!pw) return ev::undefined();
        if (a.empty()) return ev::throwTypeError("computeSkinningMatrices requires a Skeleton");
        auto* sk = hostSkeletonOf(a[0]);
        if (!sk) return ev::throwTypeError("argument must be a Skeleton");
        std::vector<float> out;
        bromesh::computeSkinningMatrices(*sk, *pw, out);
        return makeFloatArray(out);
    });
    b.def("socketWorld", 2, [](Value self, std::span<const Value> a) {
        auto* pw = hostPoseOf(self);
        if (!pw) return ev::null();
        if (a.size() < 2) return ev::throwTypeError("socketWorld requires (Skeleton, name)");
        auto* sk = hostSkeletonOf(a[0]);
        if (!sk) return ev::throwTypeError("first argument must be a Skeleton");
        std::string name = ev::toUtf8(a[1]);
        auto optM = bromesh::socketWorldMatrix(*sk, *pw, name);
        if (!optM) return ev::null();
        return makeFloatNArray(optM->data(), 16);
    });
}

void decorateRigSpecProto(ObjectBuilder& b) {
    b.accessor("name", [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        return rs ? ev::fromUtf8(rs->name) : ev::undefined();
    }, nullptr);
    b.accessor("symmetric", [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        return ev::fromBool(rs && rs->symmetric);
    }, nullptr);
    b.accessor("boneCount", [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        return rs ? ev::fromDouble(static_cast<double>(rs->bones.size())) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("landmarkCount", [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        return rs ? ev::fromDouble(static_cast<double>(rs->landmarks.size())) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("socketCount", [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        return rs ? ev::fromDouble(static_cast<double>(rs->sockets.size())) : ev::fromDouble(0.0);
    }, nullptr);
    b.def("toJSON", 0, [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        if (!rs) return ev::fromUtf8("{}");
        return ev::fromUtf8(bromesh::serializeRigSpecJSON(*rs));
    });
    b.def("landmarkNames", 0, [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        if (!rs) return makeEmptyArray();
        std::vector<std::string> names;
        names.reserve(rs->landmarks.size());
        for (const auto& l : rs->landmarks) names.push_back(l.name);
        return makeStringArray(names);
    });
    b.def("boneNames", 0, [](Value self, std::span<const Value>) {
        auto* rs = hostRigSpecOf(self);
        if (!rs) return makeEmptyArray();
        std::vector<std::string> names;
        names.reserve(rs->bones.size());
        for (const auto& b : rs->bones) names.push_back(b.name);
        return makeStringArray(names);
    });
}

void decorateVoxelChunkProto(ObjectBuilder& b) {
    b.accessor("sizeX", [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        return c ? ev::fromDouble(c->sizeX()) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("sizeY", [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        return c ? ev::fromDouble(c->sizeY()) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("sizeZ", [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        return c ? ev::fromDouble(c->sizeZ()) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("cellSize", [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        return c ? ev::fromDouble(c->cellSize()) : ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("isDirty",
        [](Value self, std::span<const Value>) {
            auto* c = hostVoxelChunkOf(self);
            return ev::fromBool(c && c->isDirty());
        },
        [](Value self, std::span<const Value> a) {
            auto* c = hostVoxelChunkOf(self);
            if (c && !a.empty()) {
                if (ev::toBool(a[0])) c->markDirty();
                else c->clearDirty();
            }
            return ev::undefined();
        });
    b.def("getVoxel", 3, [](Value self, std::span<const Value> a) {
        auto* c = hostVoxelChunkOf(self);
        if (!c || a.size() < 3) return ev::fromDouble(0.0);
        int x = static_cast<int>(ev::toDouble(a[0]));
        int y = static_cast<int>(ev::toDouble(a[1]));
        int z = static_cast<int>(ev::toDouble(a[2]));
        return ev::fromDouble(c->getVoxel(x, y, z));
    });
    b.def("setVoxel", 4, [](Value self, std::span<const Value> a) {
        auto* c = hostVoxelChunkOf(self);
        if (c && a.size() >= 4) {
            int x = static_cast<int>(ev::toDouble(a[0]));
            int y = static_cast<int>(ev::toDouble(a[1]));
            int z = static_cast<int>(ev::toDouble(a[2]));
            uint8_t m = static_cast<uint8_t>(ev::toDouble(a[3]));
            c->setVoxel(x, y, z, m);
        }
        return self;
    });
    b.def("fill", 1, [](Value self, std::span<const Value> a) {
        auto* c = hostVoxelChunkOf(self);
        if (c && !a.empty()) {
            c->fill(static_cast<uint8_t>(ev::toDouble(a[0])));
        }
        return self;
    });
    b.def("markDirty", 0, [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        if (c) c->markDirty();
        return self;
    });
    b.def("clearDirty", 0, [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        if (c) c->clearDirty();
        return self;
    });
    b.def("data", 0, [](Value self, std::span<const Value>) {
        auto* c = hostVoxelChunkOf(self);
        if (!c) return ev::undefined();
        size_t n = static_cast<size_t>(c->sizeX()) * c->sizeY() * c->sizeZ();
        return makeUint8Array(c->data(), n);
    });
    b.def("setData", 1, [](Value self, std::span<const Value> a) {
        auto* c = hostVoxelChunkOf(self);
        if (c && !a.empty()) {
            std::vector<uint8_t> bytes;
            if (readU8Vector(a[0], bytes)) {
                size_t n = static_cast<size_t>(c->sizeX()) * c->sizeY() * c->sizeZ();
                size_t copyN = std::min(bytes.size(), n);
                std::memcpy(c->data(), bytes.data(), copyN);
                c->markDirty();
            }
        }
        return self;
    });
    b.def("buildMesh", 0, [](Value self, std::span<const Value> a) {
        auto* c = hostVoxelChunkOf(self);
        if (!c) return ev::undefined();
        std::vector<float> pal;
        const float* palPtr = nullptr;
        int palCount = 0;
        if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isNull(a[0])) {
            if (!readFloatVector(a[0], pal))
                return ev::throwTypeError("palette must be Float32Array");
            palPtr = pal.data();
            palCount = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<int>(ev::toDouble(a[1]))
                                                             : static_cast<int>(pal.size() / 4);
        }
        auto mesh = c->buildMesh(palPtr, palCount);
        return wrapMesh(std::make_unique<bromesh::MeshData>(std::move(mesh)));
    });
}

Value makeIKObject() {
    ObjectBuilder ik;

    ik.def("twoBone", 6, [](Value, std::span<const Value> a) {
        if (a.size() < 6)
            return ev::throwTypeError("IK.twoBone(skel, pose, rootBone, midBone, endBone, target, pole?)");
        auto* sk = hostSkeletonOf(a[0]);
        auto* pw = hostPoseOf(a[1]);
        if (!sk || !pw)
            return ev::throwTypeError("IK.twoBone: first two args must be Skeleton and Pose");
        int r = static_cast<int>(ev::toDouble(a[2]));
        int m = static_cast<int>(ev::toDouble(a[3]));
        int e = static_cast<int>(ev::toDouble(a[4]));
        float target[3] = {0,0,0};
        if (!readVec3(a[5], target))
            return ev::throwTypeError("IK.twoBone: target must be a [x,y,z] array");
        float pole[3];
        const float* polePtr = nullptr;
        if (a.size() > 6 && !ev::isUndefined(a[6]) && !ev::isNull(a[6])) {
            if (!readVec3(a[6], pole))
                return ev::throwTypeError("IK.twoBone: pole must be a [x,y,z] array");
            polePtr = pole;
        }
        bool ok = bromesh::solveTwoBoneIK(*sk, *pw, r, m, e, target, polePtr);
        return ev::fromBool(ok);
    });

    ik.def("FABRIK", 4, [](Value, std::span<const Value> a) {
        if (a.size() < 4)
            return ev::throwTypeError("IK.FABRIK(skel, pose, chain, target, options?)");
        auto* sk = hostSkeletonOf(a[0]);
        auto* pw = hostPoseOf(a[1]);
        if (!sk || !pw)
            return ev::throwTypeError("IK.FABRIK: first two args must be Skeleton and Pose");
        if (!ev::isObject(a[2]))
            return ev::throwTypeError("IK.FABRIK: chain must be an array of bone indices");
        Value lenV = ev::getProperty(a[2], "length");
        if (!ev::isNumber(lenV))
            return ev::throwTypeError("IK.FABRIK: chain must be an array of bone indices");
        uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
        std::vector<int> chain;
        chain.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            chain.push_back(static_cast<int>(ev::toDouble(ev::getElement(a[2], i))));
        }
        float target[3] = {0,0,0};
        if (!readVec3(a[3], target))
            return ev::throwTypeError("IK.FABRIK: target must be a [x,y,z] array");
        int iterations = 10;
        float tolerance = 1e-3f;
        if (a.size() > 4 && ev::isObject(a[4])) {
            Value itV = ev::getProperty(a[4], "iterations");
            if (ev::isNumber(itV)) iterations = static_cast<int>(ev::toDouble(itV));
            Value tolV = ev::getProperty(a[4], "tolerance");
            if (ev::isNumber(tolV)) tolerance = static_cast<float>(ev::toDouble(tolV));
        }
        bool ok = bromesh::solveFABRIK(*sk, *pw, chain, target, iterations, tolerance);
        return ev::fromBool(ok);
    });

    ik.def("lookAt", 4, [](Value, std::span<const Value> a) {
        if (a.size() < 4)
            return ev::throwTypeError("IK.lookAt(skel, pose, bone, target, options?)");
        auto* sk = hostSkeletonOf(a[0]);
        auto* pw = hostPoseOf(a[1]);
        if (!sk || !pw)
            return ev::throwTypeError("IK.lookAt: first two args must be Skeleton and Pose");
        int b = static_cast<int>(ev::toDouble(a[2]));
        float target[3] = {0,0,0};
        if (!readVec3(a[3], target))
            return ev::throwTypeError("IK.lookAt: target must be a [x,y,z] array");
        float fwd[3] = {0,0,1}, up[3] = {0,1,0};
        const float* fwdPtr = nullptr;
        const float* upPtr = nullptr;
        if (a.size() > 4 && ev::isObject(a[4])) {
            Value fVal = ev::getProperty(a[4], "localForward");
            if (!ev::isUndefined(fVal) && !ev::isNull(fVal) && readVec3(fVal, fwd)) fwdPtr = fwd;
            Value uVal = ev::getProperty(a[4], "localUp");
            if (!ev::isUndefined(uVal) && !ev::isNull(uVal) && readVec3(uVal, up)) upPtr = up;
        }
        bool ok = bromesh::solveLookAt(*sk, *pw, b, target, fwdPtr, upPtr);
        return ev::fromBool(ok);
    });

    return ik.get();
}

Value makeRigObject() {
    ObjectBuilder rig;

    rig.def("spec", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("Rig.spec(name)");
        std::string name = ev::toUtf8(a[0]);
        bromesh::RigSpec s = bromesh::builtinRigSpec(name.c_str());
        return wrapRigSpec(std::move(s));
    });

    rig.def("specFromJSON", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("Rig.specFromJSON(jsonText)");
        std::string json = ev::toUtf8(a[0]);
        bromesh::RigSpec s = bromesh::parseRigSpecJSON(json.c_str());
        return wrapRigSpec(std::move(s));
    });

    rig.def("specFromFile", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("Rig.specFromFile(path)");
        std::string path = ev::toUtf8(a[0]);
        bromesh::RigSpec s = bromesh::loadRigSpecFile(path.c_str());
        return wrapRigSpec(std::move(s));
    });

    rig.def("specName", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::null();
        auto* rs = hostRigSpecOf(a[0]);
        return rs ? ev::fromUtf8(rs->name) : ev::null();
    });

    rig.def("specToJSON", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::null();
        auto* rs = hostRigSpecOf(a[0]);
        return rs ? ev::fromUtf8(bromesh::serializeRigSpecJSON(*rs)) : ev::null();
    });

    rig.def("specBoneCount", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromDouble(0.0);
        auto* rs = hostRigSpecOf(a[0]);
        return rs ? ev::fromDouble(static_cast<double>(rs->bones.size())) : ev::fromDouble(0.0);
    });

    rig.def("specLandmarkCount", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromDouble(0.0);
        auto* rs = hostRigSpecOf(a[0]);
        return rs ? ev::fromDouble(static_cast<double>(rs->landmarks.size())) : ev::fromDouble(0.0);
    });

    rig.def("detectHumanoid", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("Rig.detectHumanoid(mesh)");
        auto* m = hostMeshDataOf(a[0]);
        if (!m) return ev::throwTypeError("argument must be a Mesh");
        return makeLandmarks(bromesh::detectHumanoidLandmarks(*m));
    });

    rig.def("detectQuadruped", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::throwTypeError("Rig.detectQuadruped(mesh)");
        auto* m = hostMeshDataOf(a[0]);
        if (!m) return ev::throwTypeError("argument must be a Mesh");
        return makeLandmarks(bromesh::detectQuadrupedLandmarks(*m));
    });

    rig.def("missingLandmarks", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::throwTypeError("Rig.missingLandmarks(spec, landmarks)");
        auto* rs = hostRigSpecOf(a[0]);
        if (!rs) return ev::throwTypeError("first argument must be a RigSpec");
        auto lm = readLandmarks(a[1]);
        return makeStringArray(bromesh::missingLandmarks(*rs, lm));
    });

    rig.def("fitSkeleton", 3, [](Value, std::span<const Value> a) {
        if (a.size() < 3) return ev::throwTypeError("Rig.fitSkeleton(spec, landmarks, mesh)");
        auto* rs = hostRigSpecOf(a[0]);
        if (!rs) return ev::throwTypeError("first argument must be a RigSpec");
        auto lm = readLandmarks(a[1]);
        auto* m = hostMeshDataOf(a[2]);
        if (!m) return ev::throwTypeError("third argument must be a Mesh");
        return wrapSkeleton(bromesh::fitSkeleton(*rs, lm, *m));
    });

    rig.def("autoRig", 3, [](Value, std::span<const Value> a) {
        if (a.size() < 3) return ev::throwTypeError("Rig.autoRig(mesh, spec, landmarks, options?)");
        auto* m = hostMeshDataOf(a[0]);
        auto* rs = hostRigSpecOf(a[1]);
        if (!m) return ev::throwTypeError("mesh must be a Mesh");
        if (!rs) return ev::throwTypeError("spec must be a RigSpec");
        auto lm = readLandmarks(a[2]);
        bromesh::WeightingOptions wo;
        if (a.size() > 3 && ev::isObject(a[3])) readWeightingOptions(a[3], wo);

        auto r = bromesh::autoRig(*m, *rs, lm, wo);
        ObjectBuilder out;
        out.set("skeleton", wrapSkeleton(std::move(r.skeleton)));
        out.set("skin", wrapSkinData(std::move(r.skin)));
        out.set("missingLandmarks", makeStringArray(r.missingLandmarks));
        out.set("warnings", makeStringArray(r.warnings));
        out.set("methodUsed", ev::fromUtf8(bromesh::weightingMethodName(r.methodUsed)));
        return out.get();
    });

    rig.def("generateLocomotionCycle", 2, [](Value, std::span<const Value> a) {
        return js_rig_generateLocomotionCycle(a);
    });

    return rig.get();
}

}  // namespace

void installRiggingGlobals() {
    g_skinDataClass.install(
        "SkinData", 0,
        [](Value, std::span<const Value> a) {
            auto sd = std::make_unique<bromesh::SkinData>();
            if (!a.empty() && ev::isObject(a[0])) {
                readFloatVector(ev::getProperty(a[0], "boneWeights"), sd->boneWeights);
                readU32Vector(ev::getProperty(a[0], "boneIndices"), sd->boneIndices);
                readFloatVector(ev::getProperty(a[0], "inverseBindMatrices"), sd->inverseBindMatrices);
                Value bcV = ev::getProperty(a[0], "boneCount");
                if (ev::isNumber(bcV)) sd->boneCount = static_cast<size_t>(ev::toDouble(bcV));
                if (sd->boneCount == 0 && !sd->inverseBindMatrices.empty()) {
                    sd->boneCount = sd->inverseBindMatrices.size() / 16;
                }
            }
            return wrapSkinData(std::move(sd));
        },
        decorateSkinDataProto);

    g_skinDataClass.setStatic("validate", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("validate requires (Mesh, SkinData)");
        auto* mesh = hostMeshDataOf(a[0]);
        auto* sd = hostSkinDataOf(a[1]);
        if (!mesh || !sd) return ev::throwTypeError("validate requires (Mesh, SkinData)");

        int influences = 4;
        float sumTol = 1e-3f;
        if (a.size() > 2 && ev::isObject(a[2])) {
            Value infV = ev::getProperty(a[2], "influences");
            if (ev::isNumber(infV)) influences = static_cast<int>(ev::toDouble(infV));
            Value stV = ev::getProperty(a[2], "sumTolerance");
            if (ev::isNumber(stV)) sumTol = static_cast<float>(ev::toDouble(stV));
        }

        auto v = bromesh::validateSkin(*mesh, *sd, influences, sumTol);
        ObjectBuilder out;
        out.set("vertexCount", ev::fromDouble(static_cast<double>(v.vertexCount)));
        out.set("orphanCount", ev::fromDouble(static_cast<double>(v.orphanCount)));
        out.set("badSumCount", ev::fromDouble(static_cast<double>(v.badSumCount)));
        out.set("nanCount", ev::fromDouble(static_cast<double>(v.nanCount)));
        out.set("maxSumDeviation", ev::fromDouble(v.maxSumDeviation));
        out.set("maxInfluencesObserved", ev::fromDouble(v.maxInfluencesObserved));
        out.set("clean", ev::fromBool(v.clean()));
        return out.get();
    }, 2));

    g_skinDataClass.setStatic("transfer", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("transfer requires (targetMesh, sourceMesh, sourceSkin)");
        auto* tgt = hostMeshDataOf(a[0]);
        auto* src = hostMeshDataOf(a[1]);
        auto* sd = hostSkinDataOf(a[2]);
        if (!tgt || !src || !sd) return ev::throwTypeError("transfer requires (targetMesh, sourceMesh, sourceSkin)");
        float maxDist = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        auto out = bromesh::transferSkinWeights(*tgt, *src, *sd, maxDist);
        return wrapSkinData(std::move(out));
    }, 3));

    g_skeletonClass.install(
        "Skeleton", 0,
        [](Value, std::span<const Value> a) {
            auto s = std::make_unique<bromesh::Skeleton>();
            if (!a.empty() && ev::isObject(a[0])) {
                Value bonesV = ev::getProperty(a[0], "bones");
                if (!ev::isUndefined(bonesV)) readBonesArray(bonesV, s->bones);
                Value socksV = ev::getProperty(a[0], "sockets");
                if (!ev::isUndefined(socksV)) readSocketsArray(socksV, s->sockets);
            }
            return wrapSkeleton(std::move(s));
        },
        decorateSkeletonProto);

    g_skeletonClass.setStatic("fromBones", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        bromesh::Skeleton s;
        if (!a.empty()) readBonesArray(a[0], s.bones);
        return wrapSkeleton(std::move(s));
    }, 1));

    g_poseClass.install(
        "Pose", 0,
        [](Value, std::span<const Value> a) {
            auto p = std::make_unique<bromesh::Pose>();
            if (!a.empty()) {
                if (ev::isNumber(a[0])) {
                    int bc = static_cast<int>(ev::toDouble(a[0]));
                    p->data.assign(static_cast<size_t>(bc) * 10, 0.0f);
                    for (int i = 0; i < bc; ++i) {
                        p->data[i * 10 + 6] = 1.0f; // qw
                        p->data[i * 10 + 7] = 1.0f; // sx
                        p->data[i * 10 + 8] = 1.0f; // sy
                        p->data[i * 10 + 9] = 1.0f; // sz
                    }
                } else if (ev::isObject(a[0])) {
                    Value dataV = ev::getProperty(a[0], "data");
                    if (!ev::isUndefined(dataV)) {
                        readFloatVector(dataV, p->data);
                    } else {
                        readFloatVector(a[0], p->data);
                    }
                }
            }
            return wrapPose(std::move(p));
        },
        decoratePoseProto);

    g_poseClass.setStatic("blend", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("blend requires (poseA, poseB, weight)");
        auto* pA = hostPoseOf(a[0]);
        auto* pB = hostPoseOf(a[1]);
        if (!pA || !pB) return ev::throwTypeError("blend requires two Pose arguments");
        float weight = static_cast<float>(ev::toDouble(a[2]));
        std::vector<uint8_t> maskVec;
        const uint8_t* maskPtr = nullptr;
        if (a.size() > 3 && !ev::isUndefined(a[3]) && !ev::isNull(a[3])) {
            if (readU8Vector(a[3], maskVec) && !maskVec.empty()) maskPtr = maskVec.data();
        }
        bromesh::blendPoses(*pA, *pB, weight, maskPtr);
        return a[0];
    }, 3));

    g_poseClass.setStatic("blendN", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isObject(a[0]))
            return ev::throwTypeError("blendN(poses[], weights[], mask?)");
        Value lenV = ev::getProperty(a[0], "length");
        if (!ev::isNumber(lenV)) return ev::throwTypeError("blendN: poses must be an array");
        uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
        if (len < 1) return ev::throwTypeError("blendN: need at least one pose");

        std::vector<const bromesh::Pose*> poses(len);
        for (uint32_t i = 0; i < len; ++i) {
            Value elem = ev::getElement(a[0], i);
            auto* pw = hostPoseOf(elem);
            if (!pw) return ev::throwTypeError("blendN: poses element is not a Pose");
            poses[i] = pw;
        }

        std::vector<float> weights;
        if (!readFloatVector(a[1], weights) && ev::isObject(a[1])) {
            Value wl = ev::getProperty(a[1], "length");
            if (ev::isNumber(wl)) {
                uint32_t wn = static_cast<uint32_t>(ev::toDouble(wl));
                weights.resize(wn, 0.0f);
                for (uint32_t i = 0; i < wn; ++i) {
                    Value e = ev::getElement(a[1], i);
                    if (ev::isNumber(e)) weights[i] = static_cast<float>(ev::toDouble(e));
                }
            }
        }
        if (weights.size() != len)
            return ev::throwTypeError("blendN: weights must be an array of length poses.length");

        std::vector<uint8_t> maskVec;
        const uint8_t* maskPtr = nullptr;
        if (a.size() > 2 && !ev::isUndefined(a[2]) && !ev::isNull(a[2])) {
            if (readU8Vector(a[2], maskVec) && !maskVec.empty()) maskPtr = maskVec.data();
        }

        bromesh::Pose out;
        bromesh::blendPosesN(poses.data(), weights.data(), len, out, maskPtr);
        return wrapPose(std::move(out));
    }, 2));

    installAnimationClass(g_animationClass);

    g_rigSpecClass.install(
        "RigSpec", 0,
        [](Value, std::span<const Value>) {
            return wrapRigSpec(std::make_unique<bromesh::RigSpec>());
        },
        decorateRigSpecProto);

    g_voxelChunkClass.install(
        "VoxelChunk", 0,
        [](Value, std::span<const Value> a) {
            int sx = (!a.empty() && ev::isNumber(a[0])) ? static_cast<int>(ev::toDouble(a[0])) : 1;
            int sy = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<int>(ev::toDouble(a[1])) : 1;
            int sz = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<int>(ev::toDouble(a[2])) : 1;
            float cs = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 1.0f;
            if (sx <= 0) sx = 1;
            if (sy <= 0) sy = 1;
            if (sz <= 0) sz = 1;
            return wrapVoxelChunk(std::make_unique<bromesh::VoxelChunk>(sx, sy, sz, cs));
        },
        decorateVoxelChunkProto);

    Value ikObj = makeIKObject();
    Value rigObj = makeRigObject();

    ev::registerGlobal("IK", ikObj);
    ev::registerGlobal("Rig", rigObj);
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
