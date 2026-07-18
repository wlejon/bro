#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"
#include "physics/physics_world.h"
#include "render/skia_backend.h"
#include "dom/document.h"
#include "util/log.h"
#include "brogameagent/world.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bro::scene {


using bromath::Vec3;
using bromath::Quat;

SceneGraph::SceneGraph() {
    root_ = std::make_unique<SceneNode>("__root__");
    liveToken_ = std::make_shared<LivenessToken>();
    liveToken_->graph = this;
}


SceneGraph::~SceneGraph() {
    // Invalidate scene-as-texture links before members die: consumers in
    // other graphs resolve through this token at draw time, and nulling the
    // back-pointer makes any handle read "gone" even if a copy of the
    // shared_ptr is still held somewhere (weak_ptr expiry alone only covers
    // handles that never upgraded to shared ownership).
    if (outputTexSource_) outputTexSource_->graph = nullptr;

    // Invalidate the JS-wrapper liveness token first thing, so any wrapper
    // call that races teardown (there is none today — same thread — but the
    // ordering costs nothing) resolves to "graph gone" before nodes die.
    if (liveToken_) liveToken_->graph = nullptr;

    nodes_.clear();
}

std::shared_ptr<SceneGraph::OutputTextureSource> SceneGraph::outputTextureSource() {
    if (!outputTexSource_) {
        outputTexSource_ = std::make_shared<OutputTextureSource>();
        outputTexSource_->graph = this;
    }
    return outputTexSource_;
}


SceneNode* SceneGraph::createNode(const std::string& name) {
    auto node = std::make_unique<SceneNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

ShapeNode* SceneGraph::createShape(const std::string& name) {
    auto node = std::make_unique<ShapeNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

SpriteNode* SceneGraph::createSprite(const std::string& name) {
    auto node = std::make_unique<SpriteNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

PhysicsNode* SceneGraph::createPhysicsNode(const std::string& name) {
    auto node = std::make_unique<PhysicsNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

MeshNode* SceneGraph::createMesh(const std::string& name) {
    auto node = std::make_unique<MeshNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

SkinnedMeshNode* SceneGraph::createSkinnedMesh(const std::string& name) {
    auto node = std::make_unique<SkinnedMeshNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

InstancedMeshNode* SceneGraph::createInstancedMesh(const std::string& name) {
    auto node = std::make_unique<InstancedMeshNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

GaussianSplatNode* SceneGraph::createGaussianSplat(const std::string& name) {
    auto node = std::make_unique<GaussianSplatNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

HtmlNode* SceneGraph::createHtml(const std::string& name) {
    auto node = std::make_unique<HtmlNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

LightNode* SceneGraph::createLight(const std::string& name) {
    auto node = std::make_unique<LightNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

ParticleNode* SceneGraph::createParticles(const std::string& name) {
    auto node = std::make_unique<ParticleNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

Particles3DNode* SceneGraph::createParticles3D(const std::string& name) {
    auto node = std::make_unique<Particles3DNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

CameraNode* SceneGraph::createCamera(const std::string& name) {
    auto node = std::make_unique<CameraNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

DecalNode* SceneGraph::createDecal(const std::string& name) {
    auto node = std::make_unique<DecalNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

ReflectionProbeNode* SceneGraph::createReflectionProbe(const std::string& name) {
    auto node = std::make_unique<ReflectionProbeNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

void SceneGraph::setActiveCamera(CameraNode* cam) {
    if (!cam) {
        activeCameraId_ = 0;
        return;
    }
    // Only cameras owned by this graph can drive its view.
    if (findById(cam->id()) != cam) return;
    activeCameraId_ = cam->id();
    // Derive the view immediately so unproject/picking/readbacks issued
    // before the next tick already see the new camera.
    applyActiveCamera();
}

CameraNode* SceneGraph::activeCamera() const {
    if (!activeCameraId_) return nullptr;
    SceneNode* n = findById(activeCameraId_);
    if (!n || n->type() != SceneNode::Type::Camera) return nullptr;
    return static_cast<CameraNode*>(n);
}

void SceneGraph::applyActiveCamera() {
    if (!activeCameraId_) return;
    SceneNode* n = findById(activeCameraId_);
    if (!n || n->type() != SceneNode::Type::Camera) {
        // Active node destroyed: keep the last derived view (frozen frame
        // beats a snap to some default) and read back as "no active camera".
        activeCameraId_ = 0;
        return;
    }
    auto* cam = static_cast<CameraNode*>(n);

    // View = inverse of the camera's world transform (full 4x4 inverse, so
    // scaled ancestors don't skew the view). Camera looks down local -Z.
    const bromath::Mat4& M = n->worldMatrix();
    viewMatrix_ = bromath::minverse(M);
    cameraEye_ = {M.at(0, 3), M.at(1, 3), M.at(2, 3)};

    // Aspect: explicit when pinned on the node, else follow the canvas
    // (recomputed every apply, so resizes track automatically).
    float aspect = cam->aspect() > 0.0f
        ? cam->aspect()
        : ((canvasWidth_ > 0 && canvasHeight_ > 0)
               ? static_cast<float>(canvasWidth_) / static_cast<float>(canvasHeight_)
               : 4.0f / 3.0f);

    cameraNearZ_ = cam->nearZ();
    cameraFarZ_ = cam->farZ();
    cameraAspect_ = aspect;
    if (cam->perspective()) {
        cameraFovY_ = cam->fovY();
        projectionMatrix_ = bromath::mperspective(cam->fovY(), aspect,
                                                  cam->nearZ(), cam->farZ());
        cameraIsPerspective_ = true;
    } else {
        const float halfH = 0.5f * cam->orthoHeight();
        const float halfW = halfH * aspect;
        projectionMatrix_ = bromath::mortho(-halfW, halfW, -halfH, halfH,
                                            cam->nearZ(), cam->farZ());
        cameraIsPerspective_ = false;
    }
}

void SceneGraph::updateVisibilityGates() {
    for (auto& [id, node] : nodes_) {
        SceneNode* n = node.get();
        MeshNode* lodMesh = nullptr;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            if (m->hasLodChain()) lodMesh = m;
        }
        if (!n->hasVisibilityRange() && !lodMesh) continue;
        // One distance per node per frame: camera eye -> world origin.
        const bromath::Mat4& W = n->worldMatrix();
        const float dx = W.at(0, 3) - cameraEye_.x;
        const float dy = W.at(1, 3) - cameraEye_.y;
        const float dz = W.at(2, 3) - cameraEye_.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (n->hasVisibilityRange()) n->updateRangeGate(d);
        if (lodMesh) lodMesh->selectLodByDistance(d);
    }
}

void SceneGraph::tickAnimations(float dtSec) {
    if (dtSec <= 0.0f) return;
    // Tick the node table (independent of tree visibility, so off-screen /
    // parented-but-hidden particles still expire). Iterate an id snapshot —
    // a Particles3D onFinished callback fired below may create or destroy
    // nodes (including its own) mid-pass, invalidating map iterators.
    std::vector<uint32_t> nodeIds;
    nodeIds.reserve(nodes_.size());
    for (const auto& [id, _] : nodes_) nodeIds.push_back(id);
    for (uint32_t id : nodeIds) {
        auto it = nodes_.find(id);
        if (it == nodes_.end() || !it->second) continue;
        SceneNode* n = it->second.get();
        n->onTick(dtSec);
        // A callback fired from inside onTick (sprite animation-end) may
        // have destroyed this node — or any other — so re-resolve before
        // touching `n` again.
        it = nodes_.find(id);
        if (it == nodes_.end() || !it->second) continue;
        n = it->second.get();
        // One-shot particle completion fires after onTick returns so the
        // callback can safely destroy its own node (deferred-destroy safe:
        // `n` is never touched after the call).
        if (n->type() == SceneNode::Type::Particles3D) {
            auto* p = static_cast<Particles3DNode*>(n);
            if (p->finishedPending()) {
                auto cb = p->consumeFinishedCallback();
                if (cb) cb();
            }
        }
    }
    if (root_) root_->onTick(dtSec);

    // Tweens tick after node animations. Iterate an id snapshot — tween
    // callbacks may create or destroy tweens mid-pass — then sweep entries
    // that marked themselves destroyed (destroyTween defers the erase so a
    // tween can destroy itself from its own callback).
    if (!tweens_.empty()) {
        std::vector<uint32_t> ids;
        ids.reserve(tweens_.size());
        for (const auto& [id, _] : tweens_) ids.push_back(id);
        for (uint32_t id : ids) {
            auto it = tweens_.find(id);
            if (it == tweens_.end() || it->second->destroyed()) continue;
            it->second->tick(dtSec, *this);
        }
        for (auto it = tweens_.begin(); it != tweens_.end();) {
            if (it->second->destroyed()) it = tweens_.erase(it);
            else ++it;
        }
    }

    // Clip players tick after tweens, in creation order — the documented
    // last-writer among the animation systems. Same snapshot + deferred
    // sweep discipline as tweens (an event/finished callback may create or
    // destroy players mid-pass); ids are sorted so unordered_map iteration
    // order can't perturb the write order.
    if (!clipPlayers_.empty()) {
        auto& ids = clipPlayerIdScratch_;
        ids.clear();
        for (const auto& [id, _] : clipPlayers_) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (uint32_t id : ids) {
            auto it = clipPlayers_.find(id);
            if (it == clipPlayers_.end() || it->second->destroyed()) continue;
            it->second->tick(dtSec, *this);
        }
        for (auto it = clipPlayers_.begin(); it != clipPlayers_.end();) {
            if (it->second->destroyed()) it = clipPlayers_.erase(it);
            else ++it;
        }
    }

    advanceWindTime(dtSec);

    // Derive the view from the active camera node AFTER animations/tweens,
    // so a tweened/parented camera is applied on the same tick it moved and
    // the audio listener sync (which runs right after tickAnimations) reads
    // the fresh view. render() re-applies to catch JS mutations after tick.
    applyActiveCamera();
}

Tween* SceneGraph::createTween() {
    uint32_t id = nextTweenId_++;
    auto tween = std::make_unique<Tween>(id);
    auto* ptr = tween.get();
    tweens_[id] = std::move(tween);
    return ptr;
}

Tween* SceneGraph::findTween(uint32_t id) const {
    auto it = tweens_.find(id);
    if (it == tweens_.end() || it->second->destroyed()) return nullptr;
    return it->second.get();
}

ClipPlayer* SceneGraph::createClipPlayer() {
    uint32_t id = nextClipPlayerId_++;
    auto player = std::make_unique<ClipPlayer>(id);
    auto* ptr = player.get();
    clipPlayers_[id] = std::move(player);
    return ptr;
}

ClipPlayer* SceneGraph::findClipPlayer(uint32_t id) const {
    auto it = clipPlayers_.find(id);
    if (it == clipPlayers_.end() || it->second->destroyed()) return nullptr;
    return it->second.get();
}

void SceneGraph::destroyClipPlayer(uint32_t id) {
    auto it = clipPlayers_.find(id);
    if (it != clipPlayers_.end()) it->second->markDestroyed();
}

void SceneGraph::destroyTween(uint32_t id) {
    auto it = tweens_.find(id);
    if (it != tweens_.end()) it->second->markDestroyed();
}

void SceneGraph::setCanvasSize(int w, int h) {
    canvasWidth_ = w;
    canvasHeight_ = h;
    // If the projection was set to auto-follow the canvas aspect, recompute
    // it now so viewport and projection stay in agreement through resizes.
    // Only meaningful for perspective — orthographic callers pin their own
    // half-extents explicitly, and we don't know their intent.
    if (cameraAspectFollowsCanvas_ && cameraIsPerspective_ && w > 0 && h > 0) {
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        cameraAspect_ = aspect;
        projectionMatrix_ = bromath::mperspective(cameraFovY_, aspect,
                                              cameraNearZ_, cameraFarZ_);
    }
}

void SceneGraph::collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids) {
    for (auto* child : node->children()) {
        collectDestroyList(child, ids);
    }
    ids.push_back(node->id());
}

void SceneGraph::destroyNode(SceneNode* node) {
    if (!node || node == root_.get()) return;

    // Collect this node and all descendants
    std::vector<uint32_t> ids;
    collectDestroyList(node, ids);

    // Detach from parent first
    node->removeFromParent();

    // Destroy all collected nodes (children first due to ordering)
    for (auto id : ids) {
        agentBindings_.erase(id);
        nodes_.erase(id);
    }
}

SceneNode* SceneGraph::findById(uint32_t id) const {
    auto it = nodes_.find(id);
    return (it != nodes_.end()) ? it->second.get() : nullptr;
}

SceneNode* SceneGraph::resolveNode(uint32_t id) const {
    if (root_ && root_->id() == id) return root_.get();
    return findById(id);
}

SceneNode* SceneGraph::findByName(const std::string& name) const {
    for (auto& [id, node] : nodes_) {
        if (node->name() == name) return node.get();
    }
    return nullptr;
}

void SceneGraph::setCamera(float fovY, float aspect, float nearZ, float farZ,
                           const Vec3& eye, const Vec3& target, const Vec3& up) {
    activeCameraId_ = 0;  // imperative view wins (last camera call wins)
    projectionMatrix_ = bromath::mperspective(fovY, aspect, nearZ, farZ);
    viewMatrix_ = bromath::mlookAt(eye, target, up);
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ; cameraFovY_ = fovY; cameraAspect_ = aspect;
    cameraIsPerspective_ = true;
}

void SceneGraph::setCameraQuat(float fovY, float aspect, float nearZ, float farZ,
                               const Vec3& eye, const Quat& orientation) {
    activeCameraId_ = 0;  // imperative view wins (last camera call wins)
    projectionMatrix_ = bromath::mperspective(fovY, aspect, nearZ, farZ);
    // View matrix = inverse camera transform. For unit quaternion, inverse = conjugate.
    Quat inv = bromath::qconjugate(orientation);
    viewMatrix_ = bromath::mfromQuat(inv);
    // Translate in rotated space: view translation = inv.rotate(-eye)
    Vec3 negEye = bromath::qrotate(inv, {-eye.x, -eye.y, -eye.z});
    viewMatrix_.at(0, 3) = negEye.x;
    viewMatrix_.at(1, 3) = negEye.y;
    viewMatrix_.at(2, 3) = negEye.z;
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ; cameraFovY_ = fovY; cameraAspect_ = aspect;
    cameraIsPerspective_ = true;
}

void SceneGraph::setCameraOrtho(float left, float right, float bottom, float top,
                                float nearZ, float farZ,
                                const Vec3& eye, const Vec3& target, const Vec3& up) {
    activeCameraId_ = 0;  // imperative view wins (last camera call wins)
    projectionMatrix_ = bromath::mortho(left, right, bottom, top, nearZ, farZ);
    viewMatrix_ = bromath::mlookAt(eye, target, up);
    cameraEye_ = eye;
    cameraNearZ_ = nearZ; cameraFarZ_ = farZ;
    cameraIsPerspective_ = false;
}

void SceneGraph::setCameraPosition(float x, float y) {
    cameraX_ = x;
    cameraY_ = y;
}

void SceneGraph::setCameraZoom(float z) {
    cameraZoom_ = z;
}

void SceneGraph::syncPhysics() {
    if (!physicsWorld_) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() == SceneNode::Type::Physics) {
            auto* pn = static_cast<PhysicsNode*>(node.get());
            if (pn->autoSync()) {
                pn->syncFromPhysics(physicsWorld_);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AI integration
// ---------------------------------------------------------------------------

void SceneGraph::attachAIWorld(brogameagent::World* world, float stepHz, int maxStepsPerFrame,
                               std::shared_ptr<void> keepAlive) {
    if (!world) { aiTicker_.reset(); aiWorldPin_.reset(); return; }
    aiTicker_ = std::make_unique<AIWorldTicker>(world, stepHz, maxStepsPerFrame);
    aiWorldPin_ = std::move(keepAlive);
}

void SceneGraph::detachAIWorld() {
    aiTicker_.reset();
    aiWorldPin_.reset();
}

AgentBinding* SceneGraph::attachAgentBinding(SceneNode* node) {
    if (!node) return nullptr;
    auto it = nodes_.find(node->id());
    if (it == nodes_.end()) return nullptr;
    auto& slot = agentBindings_[node->id()];
    if (!slot) slot = std::make_unique<AgentBinding>(node);
    return slot.get();
}

AgentBinding* SceneGraph::agentBinding(uint32_t nodeId) const {
    auto it = agentBindings_.find(nodeId);
    return (it != agentBindings_.end()) ? it->second.get() : nullptr;
}

AgentBinding* SceneGraph::agentBinding(SceneNode* node) const {
    return node ? agentBinding(node->id()) : nullptr;
}

void SceneGraph::detachAgentBinding(SceneNode* node) {
    if (!node) return;
    agentBindings_.erase(node->id());
}

void SceneGraph::syncAgents(float dt) {
    // 1. Fixed-step the AI world.
    float nowSec = 0.0f;
    brogameagent::World* world = nullptr;
    if (aiTicker_) {
        aiTicker_->tick(dt);
        nowSec = aiTicker_->nowSec();
        world = aiTicker_->world();
    }

    // 2. Step each binding (think + advance + transform sync).
    // Iterate a snapshot of ids — a think() callback could theoretically
    // spawn/destroy bindings, invalidating the map's iterators.
    if (agentBindings_.empty()) return;
    std::vector<uint32_t> ids;
    ids.reserve(agentBindings_.size());
    for (const auto& [id, _] : agentBindings_) ids.push_back(id);
    for (uint32_t id : ids) {
        auto it = agentBindings_.find(id);
        if (it == agentBindings_.end()) continue;
        if (AgentBinding* b = it->second.get()) {
            b->step(world, dt, nowSec);
        }
    }
}
void SceneGraph::render() {
    // Re-derive the view from the active camera node (a JS transform write
    // between tick and render must land this frame), then run the per-frame
    // camera-distance pass that drives visibility-range gates + LOD chains.
    applyActiveCamera();
    updateVisibilityGates();

    // 2D render path (CanvasScene)
    if (canvasScene_) {
        canvasScene_->reset();
        canvasScene_->save();
        canvasScene_->translate(-cameraX_, -cameraY_);
        if (cameraZoom_ != 1.0f) {
            canvasScene_->scale(cameraZoom_, cameraZoom_);
        }
    }

    // 3D passes (mesh/instanced/splat/billboard/shadow/IBL/post) live in
    // SceneRenderer; it walks this graph via its back-reference.
    renderer_.render3D();

    // --- 2D canvas pass ---------------------------------------------------
    // Render non-world-anchored Shape/Sprite into the 2D overlay layer.
    // World-anchored nodes already rendered into the FBO above.
    {
        std::function<void(SceneNode*)> walk2D = [&](SceneNode* n) {
            if (!n->renderVisible()) return;
            if (n->type() != SceneNode::Type::Mesh &&
                n->type() != SceneNode::Type::Html &&
                !n->hasWorldAnchor()) {
                n->onRender(*this);
            }
            for (auto* c : n->children()) walk2D(c);
        };
        walk2D(root_.get());
    }

    // Restore 2D camera
    if (canvasScene_) {
        canvasScene_->restore();
    }

    // Notify the DOM element of the current FBO texture for compositing.
    // We hand over the tonemapped LDR texture; if tonemap hasn't run (no
    // 3D content this frame) we pass 0 to clear.
    if (fboTexCb_) {
        fboTexCb_(renderer_.hasMeshContent() ? renderer_.finalColorTex() : 0);
    }
}

bool SceneGraph::unprojectLocal(float localX, float localY,
                                Vec3& outOrigin, Vec3& outDir) const {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return false;
    const auto& P = projectionMatrix_;
    const auto& V = viewMatrix_;
    // View rows (camera basis in world space).
    // Row 0 = right, row 1 = up, row 2 = -forward (camera looks along -Z).
    Vec3 right  (V.at(0, 0), V.at(0, 1), V.at(0, 2));
    Vec3 up     (V.at(1, 0), V.at(1, 1), V.at(1, 2));
    Vec3 forward(-V.at(2, 0), -V.at(2, 1), -V.at(2, 2));

    float nx = (2.0f * localX / static_cast<float>(canvasWidth_)) - 1.0f;
    float ny = 1.0f - (2.0f * localY / static_cast<float>(canvasHeight_));

    if (!cameraIsPerspective_) {
        // Orthographic: rays are PARALLEL. The direction is constant (the camera
        // forward); it's the ray ORIGIN that slides across the image plane by the
        // half-extents baked into the projection matrix (P00 = 1/halfWidth,
        // P11 = 1/halfHeight, aspect already folded into halfWidth). The old
        // perspective math — fixed eye, fanned-out direction — gave wrong rays for
        // every off-centre pixel under an ortho camera, breaking click picking.
        float p00 = P.at(0, 0), p11 = P.at(1, 1);
        if (!std::isfinite(p00) || !std::isfinite(p11) || p00 == 0.0f || p11 == 0.0f) return false;
        float halfW = 1.0f / p00;
        float halfH = 1.0f / p11;
        outDir    = bromath::vnorm(forward);
        outOrigin = cameraEye_ + right * (nx * halfW) + up * (ny * halfH);
        return true;
    }

    float m11 = P.at(1, 1);
    if (!std::isfinite(m11) || m11 <= 0.0f) return false;
    float tanHalfFov = 1.0f / m11;
    float aspect = static_cast<float>(canvasWidth_) / static_cast<float>(canvasHeight_);
    Vec3 dir = forward
             + right * (nx * aspect * tanHalfFov)
             + up    * (ny * tanHalfFov);
    outDir    = bromath::vnorm(dir);
    outOrigin = cameraEye_;
    return true;
}

bool SceneGraph::pickHtmlNode(float canvasLocalX, float canvasLocalY,
                              HtmlNodePick& out) const {
    Vec3 rayOrigin, rayDir;
    if (!unprojectLocal(canvasLocalX, canvasLocalY, rayOrigin, rayDir)) {
        return false;
    }

    // Camera basis vectors — match the full-face billboard orientation
    // used in renderBillboardNode. YLock can drift the visible quad
    // slightly off the picking quad when the camera nears vertical, but
    // full-face is the common case and full-face picking is consistent
    // with what the user sees most of the time.
    const Vec3 camRight  {viewMatrix_.at(0, 0), viewMatrix_.at(0, 1), viewMatrix_.at(0, 2)};
    const Vec3 camUp     {viewMatrix_.at(1, 0), viewMatrix_.at(1, 1), viewMatrix_.at(1, 2)};
    const Vec3 quadNormal{
        camRight.y * camUp.z - camRight.z * camUp.y,
        camRight.z * camUp.x - camRight.x * camUp.z,
        camRight.x * camUp.y - camRight.y * camUp.x,
    };

    float closest = 1e30f;
    bool found = false;
    HtmlNodePick best{};

    for (auto& [id, node] : nodes_) {
        if (!node) continue;
        if (node->type() != SceneNode::Type::Html) continue;
        // renderVisible: a range-gated-out billboard isn't drawn, so it must
        // not catch clicks either.
        if (!node->renderVisible()) continue;
        if (!node->hasWorldAnchor()) continue;

        auto* hn = static_cast<HtmlNode*>(node.get());
        float ppu = hn->pxPerUnit();
        if (ppu <= 0.0f) ppu = 100.0f;
        const Vec3& scl = node->scale();
        const float halfW = 0.5f * (hn->layoutWidth()  / ppu) * scl.x;
        const float halfH = 0.5f * (hn->layoutHeight() / ppu) * scl.y;
        if (halfW <= 0.0f || halfH <= 0.0f) continue;

        const Vec3 anchor = node->worldAnchor();

        // Ray-plane intersection: t = (anchor - origin) · n / (dir · n).
        const Vec3 toAnchor{anchor.x - rayOrigin.x,
                            anchor.y - rayOrigin.y,
                            anchor.z - rayOrigin.z};
        const float denom = rayDir.x * quadNormal.x
                          + rayDir.y * quadNormal.y
                          + rayDir.z * quadNormal.z;
        if (std::fabs(denom) < 1e-6f) continue;  // ray parallel to quad
        const float t = (toAnchor.x * quadNormal.x
                       + toAnchor.y * quadNormal.y
                       + toAnchor.z * quadNormal.z) / denom;
        if (t <= 0.0f || t >= closest) continue;

        const Vec3 hitWorld{rayOrigin.x + rayDir.x * t,
                            rayOrigin.y + rayDir.y * t,
                            rayOrigin.z + rayDir.z * t};
        const Vec3 offset{hitWorld.x - anchor.x,
                          hitWorld.y - anchor.y,
                          hitWorld.z - anchor.z};
        const float u = offset.x * camRight.x + offset.y * camRight.y + offset.z * camRight.z;
        const float v = offset.x * camUp.x    + offset.y * camUp.y    + offset.z * camUp.z;
        if (std::fabs(u) > halfW || std::fabs(v) > halfH) continue;

        // Convert quad-local (u,v) in world units to top-left CSS pixels.
        // u is along camRight (positive = right); v is along camUp (positive
        // = up). The raster surface is top-down, so flip v.
        const float fx = (u / halfW) * 0.5f + 0.5f;          // 0..1, left→right
        const float fy = 0.5f - (v / halfH) * 0.5f;          // 0..1, top→bottom
        best.node = hn;
        best.localPxX = fx * hn->layoutWidth();
        best.localPxY = fy * hn->layoutHeight();
        best.distance = t;
        closest = t;
        found = true;
    }

    if (!found) return false;
    out = best;
    return true;
}

// ---------------------------------------------------------------------------
// HtmlNode rasterization — runs on the main/GL thread before scene render
// so layout of the detached Documents stays serialized with JS mutations.
// ---------------------------------------------------------------------------

void SceneGraph::materializeHtmlNodes(render::SkiaRenderer* renderer) {
    if (!renderer) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() == SceneNode::Type::Html) {
            auto* hn = static_cast<HtmlNode*>(node.get());
            hn->materializePending(renderer);
            continue;
        }
        // World-anchored sprites participate in the same 3D billboard pass
        // as HtmlNodes and need their textures uploaded on the GL/main
        // thread before scene render. Skip 2D-only sprites — those go
        // through the canvas path which doesn't need GL textures.
        if (node->type() == SceneNode::Type::Sprite && node->hasWorldAnchor()) {
            auto* sp = static_cast<SpriteNode*>(node.get());
            sp->materializeBillboard();
        }
    }
}

bool SceneGraph::hasPendingHtmlWork() const {
    for (auto& [id, node] : nodes_) {
        if (node->type() != SceneNode::Type::Html) continue;
        auto* hn = static_cast<HtmlNode*>(node.get());
        if (hn->isHtmlDirty()) return true;
        // doc_ dirty from imperative DOM mutations via node.root.
        if (hn->document() && hn->document()->isDirty()) return true;
    }
    return false;
}

} // namespace bro::scene
