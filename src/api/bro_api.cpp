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
#include <iostream>

namespace bro {

// -----------------------------------------------------------------------------
// Object3D Implementation
// -----------------------------------------------------------------------------

Object3D::Object3D(const std::string& name) : name_(name) {}

Object3D::~Object3D() = default;

void Object3D::setPosition(const Vector3& pos) {
    position_ = pos;
}

void Object3D::setPosition(float x, float y, float z) {
    position_ = Vector3(x, y, z);
}

void Object3D::setRotation(const Euler& rot) {
    rotation_ = rot;
    quaternion_ = rot.toQuaternion();
}

void Object3D::setRotation(float x, float y, float z) {
    rotation_ = Euler(x, y, z);
    quaternion_ = rotation_.toQuaternion();
}

void Object3D::setQuaternion(const Quaternion& q) {
    quaternion_ = q;
}

void Object3D::setScale(const Vector3& s) {
    scale_ = s;
}

void Object3D::setScale(float x, float y, float z) {
    scale_ = Vector3(x, y, z);
}

void Object3D::setScale(float s) {
    scale_ = Vector3(s, s, s);
}

void Object3D::add(std::shared_ptr<Object3D> child) {
    if (!child) return;
    auto existingParent = child->parent_.lock();
    if (existingParent) {
        existingParent->remove(child);
    }
    child->parent_ = shared_from_this();
    children_.push_back(child);
}

void Object3D::remove(std::shared_ptr<Object3D> child) {
    if (!child) return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
        (*it)->parent_.reset();
        children_.erase(it);
    }
}

std::shared_ptr<Object3D> Object3D::getObjectByName(const std::string& targetName) {
    if (name_ == targetName) return shared_from_this();
    for (auto& child : children_) {
        auto res = child->getObjectByName(targetName);
        if (res) return res;
    }
    return nullptr;
}

void Object3D::lookAt(const Vector3& target) {
    lookAt(target.x, target.y, target.z);
}

void Object3D::lookAt(float tx, float ty, float tz) {
    Vector3 dir(tx - position_.x, ty - position_.y, tz - position_.z);
    if (dir.lengthSq() < 1e-6f) return;
    dir.normalize();

    Vector3 up(0, 1, 0);
    if (std::abs(dir.dot(up)) > 0.999f) {
        up = Vector3(0, 0, 1);
    }

    Vector3 right = up.cross(dir).normalized();
    Vector3 actualUp = dir.cross(right).normalized();

    // Compute pitch and yaw
    float yaw = std::atan2(-dir.x, -dir.z);
    float pitch = std::asin(dir.y);
    rotation_ = Euler(pitch, yaw, 0.0f);
    quaternion_ = rotation_.toQuaternion();
}

// -----------------------------------------------------------------------------
// Geometry Implementation
// -----------------------------------------------------------------------------

struct Geometry::Impl {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
};

Geometry::Geometry(GeometryType type) : type_(type), impl_(std::make_unique<Impl>()) {}

Geometry::~Geometry() = default;

std::shared_ptr<Geometry> Geometry::createBox(float width, float height, float depth) {
    auto geo = std::make_shared<Geometry>(GeometryType::Box);
    float hw = width * 0.5f;
    float hh = height * 0.5f;
    float hd = depth * 0.5f;

    // Standard box vertices
    geo->impl_->positions = {
        // Front
        -hw, -hh,  hd,   hw, -hh,  hd,   hw,  hh,  hd,  -hw,  hh,  hd,
        // Back
         hw, -hh, -hd,  -hw, -hh, -hd,  -hw,  hh, -hd,   hw,  hh, -hd,
        // Top
        -hw,  hh,  hd,   hw,  hh,  hd,   hw,  hh, -hd,  -hw,  hh, -hd,
        // Bottom
        -hw, -hh, -hd,   hw, -hh, -hd,   hw, -hh,  hd,  -hw, -hh,  hd,
        // Right
         hw, -hh,  hd,   hw, -hh, -hd,   hw,  hh, -hd,   hw,  hh,  hd,
        // Left
        -hw, -hh, -hd,  -hw, -hh,  hd,  -hw,  hh,  hd,  -hw,  hh, -hd
    };

    geo->impl_->normals = {
        // Front
         0,  0,  1,   0,  0,  1,   0,  0,  1,   0,  0,  1,
        // Back
         0,  0, -1,   0,  0, -1,   0,  0, -1,   0,  0, -1,
        // Top
         0,  1,  0,   0,  1,  0,   0,  1,  0,   0,  1,  0,
        // Bottom
         0, -1,  0,   0, -1,  0,   0, -1,  0,   0, -1,  0,
        // Right
         1,  0,  0,   1,  0,  0,   1,  0,  0,   1,  0,  0,
        // Left
        -1,  0,  0,  -1,  0,  0,  -1,  0,  0,  -1,  0,  0
    };

    geo->impl_->uvs = {
        0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1
    };

    geo->impl_->indices = {
         0,  1,  2,   0,  2,  3,
         4,  5,  6,   4,  6,  7,
         8,  9, 10,   8, 10, 11,
        12, 13, 14,  12, 14, 15,
        16, 17, 18,  16, 18, 19,
        20, 21, 22,  20, 22, 23
    };

    return geo;
}

std::shared_ptr<Geometry> Geometry::createSphere(float radius, int widthSegments, int heightSegments) {
    auto geo = std::make_shared<Geometry>(GeometryType::Sphere);

    for (int y = 0; y <= heightSegments; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(heightSegments);
        float phi = v * static_cast<float>(M_PI);

        for (int x = 0; x <= widthSegments; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(widthSegments);
            float theta = u * static_cast<float>(M_PI) * 2.0f;

            float px = -radius * std::cos(theta) * std::sin(phi);
            float py = radius * std::cos(phi);
            float pz = radius * std::sin(theta) * std::sin(phi);

            geo->impl_->positions.push_back(px);
            geo->impl_->positions.push_back(py);
            geo->impl_->positions.push_back(pz);

            Vector3 n(px, py, pz);
            n.normalize();
            geo->impl_->normals.push_back(n.x);
            geo->impl_->normals.push_back(n.y);
            geo->impl_->normals.push_back(n.z);

            geo->impl_->uvs.push_back(u);
            geo->impl_->uvs.push_back(1.0f - v);
        }
    }

    for (int y = 0; y < heightSegments; ++y) {
        for (int x = 0; x < widthSegments; ++x) {
            uint32_t first = (y * (widthSegments + 1)) + x;
            uint32_t second = first + widthSegments + 1;

            geo->impl_->indices.push_back(first);
            geo->impl_->indices.push_back(second);
            geo->impl_->indices.push_back(first + 1);

            geo->impl_->indices.push_back(second);
            geo->impl_->indices.push_back(second + 1);
            geo->impl_->indices.push_back(first + 1);
        }
    }

    return geo;
}

std::shared_ptr<Geometry> Geometry::createPlane(float width, float height) {
    auto geo = std::make_shared<Geometry>(GeometryType::Plane);
    float hw = width * 0.5f;
    float hh = height * 0.5f;

    geo->impl_->positions = {
        -hw, -hh, 0.0f,
         hw, -hh, 0.0f,
         hw,  hh, 0.0f,
        -hw,  hh, 0.0f
    };

    geo->impl_->normals = {
        0, 0, 1,
        0, 0, 1,
        0, 0, 1,
        0, 0, 1
    };

    geo->impl_->uvs = {
        0, 0,
        1, 0,
        1, 1,
        0, 1
    };

    geo->impl_->indices = {
        0, 1, 2,
        0, 2, 3
    };

    return geo;
}

void Geometry::setPositions(const std::vector<float>& positions) { impl_->positions = positions; }
void Geometry::setNormals(const std::vector<float>& normals) { impl_->normals = normals; }
void Geometry::setUVs(const std::vector<float>& uvs) { impl_->uvs = uvs; }
void Geometry::setIndices(const std::vector<uint32_t>& indices) { impl_->indices = indices; }

const std::vector<float>& Geometry::positions() const { return impl_->positions; }
const std::vector<float>& Geometry::normals() const { return impl_->normals; }
const std::vector<float>& Geometry::uvs() const { return impl_->uvs; }
const std::vector<uint32_t>& Geometry::indices() const { return impl_->indices; }

// -----------------------------------------------------------------------------
// Material Implementation
// -----------------------------------------------------------------------------

Material::Material() = default;
Material::~Material() = default;

// -----------------------------------------------------------------------------
// Mesh Implementation
// -----------------------------------------------------------------------------

Mesh::Mesh(std::shared_ptr<Geometry> geometry, std::shared_ptr<Material> material, const std::string& name)
    : Object3D(name), geometry_(geometry), material_(material) {}

Mesh::~Mesh() = default;

// -----------------------------------------------------------------------------
// Camera Implementation
// -----------------------------------------------------------------------------

PerspectiveCamera::PerspectiveCamera(float fov, float aspect, float nearZ, float farZ, const std::string& name)
    : Camera(name), fov_(fov), aspect_(aspect) {
    nearZ_ = nearZ;
    farZ_ = farZ;
}

// -----------------------------------------------------------------------------
// Light Implementation
// -----------------------------------------------------------------------------

Light::Light(const Color& color, float intensity, const std::string& name)
    : Object3D(name), color_(color), intensity_(intensity) {}

AmbientLight::AmbientLight(const Color& color, float intensity, const std::string& name)
    : Light(color, intensity, name) {}

DirectionalLight::DirectionalLight(const Color& color, float intensity, const std::string& name)
    : Light(color, intensity, name) {}

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
            float fovRad = pCam.fov() * static_cast<float>(M_PI) / 180.0f;
            Vector3 target(pos.x + 0.0f, pos.y + 0.0f, pos.z - 1.0f);
            graph->setCamera(fovRad, pCam.aspect(), pCam.nearZ(), pCam.farZ(),
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
