#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <bro/bro_api.hpp>

#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "scene/light_node.h"
#include "scene/camera_node.h"
#include "scene/scene_renderer.h"
#include "physics/physics_world.h"

#include <bromesh/mesh_data.h>
#include <bromath/vec.h>
#include <bromath/quat.h>

#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace bro {

// -----------------------------------------------------------------------------
// Scene Implementation
// -----------------------------------------------------------------------------

struct Scene::Impl {
    std::unique_ptr<bro::scene::SceneGraph> sceneGraph;

    Impl() : sceneGraph(std::make_unique<bro::scene::SceneGraph>()) {}
};

Scene::Scene(const std::string& name) : Object3D(name), impl_(std::make_unique<Impl>()) {}

Scene::~Scene() = default;

// -----------------------------------------------------------------------------
// WebGLRenderer Implementation
// -----------------------------------------------------------------------------

struct WebGLRenderer::Impl {
    std::unordered_map<uint64_t, bro::scene::MeshNode*> nodeMap;

    void syncSceneToGraph(Scene& scene, Camera& camera, bro::scene::SceneGraph* graph) {
        if (!graph) return;

        // Set ambient background lighting
        auto bg = scene.background();
        graph->setAmbient(bg.r, bg.g, bg.b);

        // Sync camera
        auto pos = camera.position();
        if (camera.type() == ObjectType::PerspectiveCamera) {
            auto& pCam = static_cast<PerspectiveCamera&>(camera);
            float fovRad = pCam.fov * static_cast<float>(M_PI) / 180.0f;
            Vector3 target = camera.target();
            graph->setCamera(fovRad, pCam.aspect, pCam.nearZ(), pCam.farZ(),
                             bromath::Vec3(pos.x, pos.y, pos.z),
                             bromath::Vec3(target.x, target.y, target.z));
        }

        // Helper recursive traversal
        std::function<void(Object3D&)> syncObject = [&](Object3D& obj) {
            if (!obj.visible()) return;

            if (obj.type() == ObjectType::Mesh) {
                auto& meshObj = static_cast<Mesh&>(obj);
                uint64_t ptrKey = reinterpret_cast<uint64_t>(&meshObj);

                bro::scene::MeshNode* meshNode = nullptr;
                auto it = nodeMap.find(ptrKey);
                if (it != nodeMap.end()) {
                    meshNode = it->second;
                } else {
                    meshNode = graph->createMesh(meshObj.name());
                    nodeMap[ptrKey] = meshNode;
                }

                if (meshNode) {
                    auto mpos = meshObj.position();
                    auto mrot = meshObj.quaternion();
                    auto mscale = meshObj.scale();

                    meshNode->setPosition(bromath::Vec3(mpos.x, mpos.y, mpos.z));
                    meshNode->setRotation(bromath::Quat(mrot.x, mrot.y, mrot.z, mrot.w));
                    meshNode->setScale(bromath::Vec3(mscale.x, mscale.y, mscale.z));

                    // Build bromesh::MeshData if geometry exists
                    auto geo = meshObj.geometry();
                    auto mat = meshObj.material();
                    if (geo) {
                        bromesh::MeshData meshData;
                        meshData.positions = geo->positions();
                        meshData.normals = geo->normals();
                        meshData.uvs = geo->uvs();
                        meshData.indices = geo->indices();
                        meshNode->setMesh(meshData);
                    }

                    if (mat) {
                        auto c = mat->color();
                        meshNode->setColor(c.r, c.g, c.b, c.a);
                        meshNode->setRoughness(mat->roughness());
                        meshNode->setMetallic(mat->metalness());
                    }
                }
            } else if (obj.type() == ObjectType::DirectionalLight) {
                auto& lightObj = static_cast<DirectionalLight&>(obj);
                auto lightNode = graph->createLight(lightObj.name());
                lightNode->setKind(bro::scene::LightNode::Kind::Directional);
                auto c = lightObj.color();
                lightNode->setColor(c.r, c.g, c.b);
                lightNode->setIntensity(lightObj.intensity());
                lightNode->setCastsShadow(lightObj.castsShadow());
            }

            for (auto& child : obj.children()) {
                if (child) syncObject(*child);
            }
        };

        syncObject(scene);
    }
};

WebGLRenderer::WebGLRenderer(int width, int height)
    : width_(width), height_(height), impl_(std::make_unique<Impl>()) {}

WebGLRenderer::~WebGLRenderer() = default;

void WebGLRenderer::setSize(int width, int height) {
    width_ = width;
    height_ = height;
}

void WebGLRenderer::setClearColor(const Color& color, float alpha) {
    clearColor_ = Color(color.r, color.g, color.b, alpha);
}

void WebGLRenderer::render(Scene& scene, Camera& camera) {
    if (!scene.getImpl() || !scene.getImpl()->sceneGraph) return;
    auto graph = scene.getImpl()->sceneGraph.get();
    graph->setCanvasSize(width_, height_);

    impl_->syncSceneToGraph(scene, camera, graph);
    graph->render();
}

void WebGLRenderer::render(std::shared_ptr<Scene> scene, std::shared_ptr<Camera> camera) {
    if (scene && camera) {
        render(*scene, *camera);
    }
}

// -----------------------------------------------------------------------------
// Render Callback
// -----------------------------------------------------------------------------

static std::function<void(double dt)> g_renderCallback;

void set_render_callback(std::function<void(double dt)> callback) {
    g_renderCallback = std::move(callback);
}

std::function<void(double dt)> get_render_callback() {
    return g_renderCallback;
}

void tick_render_callback(double dt) {
    if (g_renderCallback) {
        g_renderCallback(dt);
    }
}

// -----------------------------------------------------------------------------
// RigidBody Implementation
// -----------------------------------------------------------------------------

struct RigidBody::Impl {
    Vector3 position{0, 0, 0};
    Quaternion rotation{0, 0, 0, 1};
    Vector3 linearVelocity{0, 0, 0};
    Vector3 angularVelocity{0, 0, 0};
};

RigidBody::RigidBody(RigidBodyType bodyType, float mass)
    : bodyType_(bodyType), mass_(mass), impl_(std::make_unique<Impl>()) {}

RigidBody::~RigidBody() = default;

Vector3 RigidBody::position() const { return impl_->position; }
void RigidBody::setPosition(const Vector3& pos) { impl_->position = pos; }

Quaternion RigidBody::rotation() const { return impl_->rotation; }
void RigidBody::setRotation(const Quaternion& rot) { impl_->rotation = rot; }

Vector3 RigidBody::linearVelocity() const { return impl_->linearVelocity; }
void RigidBody::setLinearVelocity(const Vector3& vel) { impl_->linearVelocity = vel; }

Vector3 RigidBody::angularVelocity() const { return impl_->angularVelocity; }
void RigidBody::setAngularVelocity(const Vector3& vel) { impl_->angularVelocity = vel; }

void RigidBody::addForce(const Vector3& force) {
    impl_->linearVelocity += (force / (mass_ > 0.0f ? mass_ : 1.0f));
}

void RigidBody::addImpulse(const Vector3& impulse) {
    impl_->linearVelocity += (impulse / (mass_ > 0.0f ? mass_ : 1.0f));
}

void RigidBody::setFriction(float f) { friction_ = f; }
void RigidBody::setRestitution(float r) { restitution_ = r; }

// -----------------------------------------------------------------------------
// PhysicsWorld Implementation
// -----------------------------------------------------------------------------

struct PhysicsWorld::Impl {
    Vector3 gravity{0.0f, -9.81f, 0.0f};
    std::vector<std::shared_ptr<RigidBody>> bodies;
    std::unique_ptr<bro::physics::PhysicsWorld> internalPhysics;

    Impl() : internalPhysics(std::make_unique<bro::physics::PhysicsWorld>()) {}
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::setGravity(const Vector3& gravity) {
    impl_->gravity = gravity;
}

Vector3 PhysicsWorld::gravity() const {
    return impl_->gravity;
}

void PhysicsWorld::step(double dt) {
    float dtSec = static_cast<float>(dt);

    for (auto& body : impl_->bodies) {
        if (!body || body->bodyType() == RigidBodyType::Static) continue;

        // Apply gravity
        auto vel = body->linearVelocity();
        vel += impl_->gravity * dtSec;
        body->setLinearVelocity(vel);

        // Integrate position
        auto pos = body->position();
        pos += vel * dtSec;
        body->setPosition(pos);
    }
}

void PhysicsWorld::addBody(std::shared_ptr<RigidBody> body) {
    if (!body) return;
    auto it = std::find(impl_->bodies.begin(), impl_->bodies.end(), body);
    if (it == impl_->bodies.end()) {
        impl_->bodies.push_back(body);
    }
}

void PhysicsWorld::removeBody(std::shared_ptr<RigidBody> body) {
    if (!body) return;
    auto it = std::find(impl_->bodies.begin(), impl_->bodies.end(), body);
    if (it != impl_->bodies.end()) {
        impl_->bodies.erase(it);
    }
}

const std::vector<std::shared_ptr<RigidBody>>& PhysicsWorld::bodies() const {
    return impl_->bodies;
}

RaycastHit PhysicsWorld::raycast(const Vector3& origin, const Vector3& direction, float maxDistance) {
    RaycastHit result;
    result.hit = false;
    float closestDist = maxDistance;

    Vector3 dirNorm = direction.normalized();

    for (auto& body : impl_->bodies) {
        if (!body) continue;
        Vector3 bpos = body->position();
        Vector3 toBody = bpos - origin;
        float proj = toBody.dot(dirNorm);

        if (proj > 0.0f && proj < closestDist) {
            Vector3 projPoint = origin + (dirNorm * proj);
            float distSq = (projPoint - bpos).lengthSq();

            // Simple sphere bounding check (radius 1.0)
            if (distSq <= 1.0f) {
                result.hit = true;
                result.distance = proj;
                result.point = projPoint;
                result.normal = (projPoint - bpos).normalized();
                result.body = body;
                closestDist = proj;
            }
        }
    }

    return result;
}

} // namespace bro
