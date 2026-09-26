// physics_penetration.cpp — Native penetration query and batch transform update.

#include "physics/physics_world.h"

#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyInterface.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace bro::physics {

namespace {

uint32_t getSubShapeIndex(const JPH::Shape* shape, const JPH::SubShapeID& id) {
    while (shape && shape->GetType() == JPH::EShapeType::Decorated) {
        shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
    }
    if (shape && shape->GetType() == JPH::EShapeType::Compound) {
        const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
        JPH::SubShapeID remainder;
        return compound->GetSubShapeIndexFromID(id, remainder);
    }
    return 0;
}

struct HitRecord {
    uint32_t sub1;
    uint32_t sub2;
    float depth;
    JPH::Vec3 position;
};

class PairPenetrationCollector : public JPH::CollideShapeCollector {
public:
    PairPenetrationCollector(float minDepth, bool keepSeparated)
        : mMinDepth(minDepth), mKeepSeparated(keepSeparated) {
        hits.reserve(16);
    }

    void reset(const JPH::Shape* s1, const JPH::Shape* s2, JPH::BodyID id1, JPH::BodyID id2, JPH::RVec3Arg baseOffset) {
        mShape1 = s1;
        mShape2 = s2;
        mBody1 = id1;
        mBody2 = id2;
        mBaseOffset = baseOffset;
        hits.clear();
        ResetEarlyOutFraction();
    }

    void AddHit(const JPH::CollideShapeResult& inResult) override {
        if (inResult.mPenetrationDepth < mMinDepth) return;
        if (!mKeepSeparated && inResult.mPenetrationDepth <= 0.0f) return;

        uint32_t sub1 = getSubShapeIndex(mShape1, inResult.mSubShapeID1);
        uint32_t sub2 = getSubShapeIndex(mShape2, inResult.mSubShapeID2);

        JPH::Vec3 contact = JPH::Vec3(mBaseOffset) + 0.5f * (inResult.mContactPointOn1 + inResult.mContactPointOn2);

        for (auto& h : hits) {
            if (h.sub1 == sub1 && h.sub2 == sub2) {
                if (inResult.mPenetrationDepth > h.depth) {
                    h.depth = inResult.mPenetrationDepth;
                    h.position = contact;
                }
                return;
            }
        }

        hits.push_back({ sub1, sub2, inResult.mPenetrationDepth, contact });
    }

    float mMinDepth = 0.0f;
    bool mKeepSeparated = false;
    const JPH::Shape* mShape1 = nullptr;
    const JPH::Shape* mShape2 = nullptr;
    JPH::BodyID mBody1;
    JPH::BodyID mBody2;
    JPH::RVec3 mBaseOffset;
    std::vector<HitRecord> hits;
};

} // namespace

void PhysicsWorld::setTransform(JPH::BodyID id, JPH::RVec3 pos, JPH::Quat rot) {
    if (id.IsInvalid()) return;
    physicsSystem_.GetBodyInterface().SetPositionAndRotationWhenChanged(
        id, pos, rot, JPH::EActivation::DontActivate);
}

void PhysicsWorld::setTransforms(const std::vector<BodyTransformUpdate>& updates) {
    auto& bi = physicsSystem_.GetBodyInterface();
    for (const auto& u : updates) {
        if (u.id.IsInvalid()) continue;
        bi.SetPositionAndRotationWhenChanged(u.id, u.position, u.rotation, JPH::EActivation::DontActivate);
        const uint32_t key = u.id.GetIndexAndSequenceNumber();
        if (u.scale.IsClose(JPH::Vec3::sOne(), 1e-12f)) queryScale_.erase(key);
        else queryScale_[key] = u.scale;
    }
}

std::vector<PhysicsWorld::PenetrationHit> PhysicsWorld::penetrations(const PenetrationOptions& options) const {
    std::vector<PenetrationHit> results;

    std::vector<JPH::BodyID> bodyIds = options.bodies;
    if (bodyIds.empty()) {
        JPH::BodyIDVector allBodies;
        physicsSystem_.GetBodies(allBodies);
        bodyIds.assign(allBodies.begin(), allBodies.end());
    }
    if (bodyIds.size() < 2) return results;

    bodyIds.erase(std::remove_if(bodyIds.begin(), bodyIds.end(),
        [](const JPH::BodyID& id) { return id.IsInvalid(); }), bodyIds.end());
    if (bodyIds.size() < 2) return results;

    std::unordered_set<uint64_t> ignorePairs;
    ignorePairs.reserve(options.ignorePairs.size());
    for (const auto& pair : options.ignorePairs) {
        if (pair.first.IsInvalid() || pair.second.IsInvalid()) continue;
        uint32_t a = pair.first.GetIndexAndSequenceNumber();
        uint32_t b = pair.second.GetIndexAndSequenceNumber();
        if (a > b) std::swap(a, b);
        ignorePairs.insert((uint64_t(a) << 32) | uint64_t(b));
    }

    const JPH::BodyLockInterface& li = isIdle()
        ? static_cast<const JPH::BodyLockInterface&>(physicsSystem_.GetBodyLockInterfaceNoLock())
        : physicsSystem_.GetBodyLockInterface();

    JPH::BodyLockMultiRead lock(li, bodyIds.data(), static_cast<int>(bodyIds.size()));

    struct BodyBox {
        JPH::BodyID id;
        const JPH::Body* body = nullptr;
        JPH::AABox box;
        JPH::Vec3 scale;
        JPH::RMat44 com;
    };

    std::vector<BodyBox> boxes;
    boxes.reserve(bodyIds.size());

    for (int i = 0; i < lock.GetNumBodies(); ++i) {
        const JPH::Body* body = lock.GetBody(i);
        if (!body || body->GetShape() == nullptr) continue;
        if (options.layerMask != 0) {
            uint32_t layer = body->GetObjectLayer();
            if (layer < 32 && !(options.layerMask & (1u << layer))) continue;
        }
        const JPH::Shape* shape = body->GetShape();
        JPH::Vec3 scale = JPH::Vec3::sOne();
        auto it = queryScale_.find(body->GetID().GetIndexAndSequenceNumber());
        if (it != queryScale_.end()) scale = shape->MakeScaleValid(it->second);
        // The shape's geometry is centre-of-mass relative, and scale acts
        // about the body origin, so the scaled COM sits at scale * COM.
        JPH::RMat44 com = body->GetWorldTransform().PreTranslated(scale * shape->GetCenterOfMass());
        JPH::AABox box = shape->GetWorldSpaceBounds(com, scale);
        if (options.maxSeparation > 0.0f) {
            box.ExpandBy(JPH::Vec3::sReplicate(options.maxSeparation));
        }
        boxes.push_back({ body->GetID(), body, box, scale, com });
    }

    if (boxes.size() < 2) return results;

    std::sort(boxes.begin(), boxes.end(), [](const BodyBox& a, const BodyBox& b) {
        return a.box.mMin.GetX() < b.box.mMin.GetX();
    });

    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = options.maxSeparation;

    PairPenetrationCollector collector(options.minDepth, options.maxSeparation > 0.0f);

    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& a = boxes[i];
        float maxX = a.box.mMax.GetX();

        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const auto& b = boxes[j];
            if (b.box.mMin.GetX() > maxX) {
                break;
            }

            if (a.id == b.id) continue;

            if (!a.box.Overlaps(b.box)) continue;

            uint32_t idA = a.id.GetIndexAndSequenceNumber();
            uint32_t idB = b.id.GetIndexAndSequenceNumber();
            uint32_t minId = std::min(idA, idB);
            uint32_t maxId = std::max(idA, idB);
            uint64_t pairKey = (uint64_t(minId) << 32) | uint64_t(maxId);
            if (ignorePairs.contains(pairKey)) continue;

            const BodyBox* e1 = &a;
            const BodyBox* e2 = &b;
            if (idA > idB) {
                std::swap(e1, e2);
            }
            const JPH::Body* b1 = e1->body;
            const JPH::Body* b2 = e2->body;

            JPH::RVec3 baseOffset = e1->com.GetTranslation();
            collector.reset(b1->GetShape(), b2->GetShape(), b1->GetID(), b2->GetID(), baseOffset);

            JPH::Mat44 transform1 = e1->com.PostTranslated(-baseOffset).ToMat44();
            JPH::Mat44 transform2 = e2->com.PostTranslated(-baseOffset).ToMat44();

            JPH::SubShapeIDCreator subId1, subId2;
            JPH::CollisionDispatch::sCollideShapeVsShape(
                b1->GetShape(), b2->GetShape(),
                e1->scale, e2->scale,
                transform1, transform2,
                subId1, subId2,
                settings,
                collector
            );

            for (const auto& rec : collector.hits) {
                PenetrationHit hit;
                hit.body1 = b1->GetID();
                hit.subShape1 = rec.sub1;
                hit.body2 = b2->GetID();
                hit.subShape2 = rec.sub2;
                hit.depth = rec.depth;
                hit.position = rec.position;
                results.push_back(hit);
            }
        }
    }

    return results;
}

} // namespace bro::physics
