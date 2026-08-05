#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace bro {

// Forward declarations
class Object3D;
class Scene;
class Geometry;
class Material;
class Mesh;
class Camera;
class PerspectiveCamera;
class Light;
class AmbientLight;
class DirectionalLight;
class Group;
class WebGLRenderer;
class RigidBody;
class PhysicsWorld;

// -----------------------------------------------------------------------------
// Data Types (Value Structs)
// -----------------------------------------------------------------------------

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vector3() = default;
    constexpr Vector3(float px, float py, float pz) : x(px), y(py), z(pz) {}

    Vector3 operator+(const Vector3& rhs) const { return Vector3(x + rhs.x, y + rhs.y, z + rhs.z); }
    Vector3 operator-(const Vector3& rhs) const { return Vector3(x - rhs.x, y - rhs.y, z - rhs.z); }
    Vector3 operator*(float scalar) const { return Vector3(x * scalar, y * scalar, z * scalar); }
    Vector3 operator/(float scalar) const { return Vector3(x / scalar, y / scalar, z / scalar); }

    Vector3& operator+=(const Vector3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    Vector3& operator-=(const Vector3& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }
    Vector3& operator*=(float scalar) { x *= scalar; y *= scalar; z *= scalar; return *this; }
    Vector3& operator/=(float scalar) { x /= scalar; y /= scalar; z /= scalar; return *this; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float lengthSq() const { return x * x + y * y + z * z; }
    Vector3 normalized() const {
        float len = length();
        return len > 0.0f ? *this / len : Vector3(0, 0, 0);
    }
    void normalize() {
        float len = length();
        if (len > 0.0f) { x /= len; y /= len; z /= len; }
    }
    float dot(const Vector3& rhs) const { return x * rhs.x + y * rhs.y + z * rhs.z; }
    Vector3 cross(const Vector3& rhs) const {
        return Vector3(
            y * rhs.z - z * rhs.y,
            z * rhs.x - x * rhs.z,
            x * rhs.y - y * rhs.x
        );
    }
    void set(float px, float py, float pz) { x = px; y = py; z = pz; }
    float distanceTo(const Vector3& v) const { return (*this - v).length(); }
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr Color() = default;
    constexpr Color(float pr, float pg, float pb, float pa = 1.0f) : r(pr), g(pg), b(pb), a(pa) {}
    explicit Color(uint32_t hex, float pa = 1.0f)
        : r(((hex >> 16) & 0xFF) / 255.0f),
          g(((hex >> 8) & 0xFF) / 255.0f),
          b((hex & 0xFF) / 255.0f),
          a(pa) {}

    void setRGB(float pr, float pg, float pb) { r = pr; g = pg; b = pb; }
    void setHex(uint32_t hex) {
        r = ((hex >> 16) & 0xFF) / 255.0f;
        g = ((hex >> 8) & 0xFF) / 255.0f;
        b = (hex & 0xFF) / 255.0f;
    }
};

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quaternion() = default;
    constexpr Quaternion(float px, float py, float pz, float pw) : x(px), y(py), z(pz), w(pw) {}

    static Quaternion fromAxisAngle(const Vector3& axis, float angle) {
        float halfAngle = angle * 0.5f;
        float s = std::sin(halfAngle);
        Vector3 normAxis = axis.normalized();
        return Quaternion(normAxis.x * s, normAxis.y * s, normAxis.z * s, std::cos(halfAngle));
    }

    static Quaternion fromEuler(float rx, float ry, float rz) {
        float c1 = std::cos(rx * 0.5f);
        float s1 = std::sin(rx * 0.5f);
        float c2 = std::cos(ry * 0.5f);
        float s2 = std::sin(ry * 0.5f);
        float c3 = std::cos(rz * 0.5f);
        float s3 = std::sin(rz * 0.5f);

        return Quaternion(
            s1 * c2 * c3 + c1 * s2 * s3,
            c1 * s2 * c3 - s1 * c2 * s3,
            c1 * c2 * s3 + s1 * s2 * c3,
            c1 * c2 * c3 - s1 * s2 * s3
        );
    }

    Quaternion operator*(const Quaternion& rhs) const {
        return Quaternion(
            w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
            w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
            w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w,
            w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z
        );
    }

    Vector3 rotateVector(const Vector3& v) const {
        Vector3 qv(x, y, z);
        Vector3 uv = qv.cross(v);
        Vector3 uuv = qv.cross(uv);
        return v + (uv * (2.0f * w)) + (uuv * 2.0f);
    }

    void set(float px, float py, float pz, float pw) { x = px; y = py; z = pz; w = pw; }
};

struct Euler {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string order = "XYZ";

    Euler() = default;
    Euler(float px, float py, float pz, std::string pOrder = "XYZ")
        : x(px), y(py), z(pz), order(std::move(pOrder)) {}

    Quaternion toQuaternion() const {
        return Quaternion::fromEuler(x, y, z);
    }

    void set(float px, float py, float pz, std::string pOrder = "XYZ") {
        x = px; y = py; z = pz; order = std::move(pOrder);
    }
};

struct Matrix4 {
    std::array<float, 16> elements = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    Matrix4() = default;
    static Matrix4 identity() { return Matrix4(); }

    static Matrix4 makeTranslation(float px, float py, float pz) {
        Matrix4 m;
        m.elements[12] = px;
        m.elements[13] = py;
        m.elements[14] = pz;
        return m;
    }

    static Matrix4 makeScale(float sx, float sy, float sz) {
        Matrix4 m;
        m.elements[0] = sx;
        m.elements[5] = sy;
        m.elements[10] = sz;
        return m;
    }

    static Matrix4 makePerspective(float fovRad, float aspect, float nearZ, float farZ) {
        Matrix4 m;
        float tanHalfFov = std::tan(fovRad * 0.5f);
        std::fill(m.elements.begin(), m.elements.end(), 0.0f);
        m.elements[0] = 1.0f / (aspect * tanHalfFov);
        m.elements[5] = 1.0f / tanHalfFov;
        m.elements[10] = -(farZ + nearZ) / (farZ - nearZ);
        m.elements[11] = -1.0f;
        m.elements[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
        return m;
    }
};

// -----------------------------------------------------------------------------
// Scene Graph Types
// -----------------------------------------------------------------------------

enum class ObjectType {
    Object3D,
    Scene,
    Mesh,
    Group,
    Camera,
    PerspectiveCamera,
    Light,
    AmbientLight,
    DirectionalLight
};

class Object3D : public std::enable_shared_from_this<Object3D> {
public:
    Object3D(const std::string& name = "");
    virtual ~Object3D();

    virtual ObjectType type() const { return ObjectType::Object3D; }

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    const Vector3& position() const { return position_; }
    void setPosition(const Vector3& pos);
    void setPosition(float x, float y, float z);

    const Euler& rotation() const { return rotation_; }
    void setRotation(const Euler& rot);
    void setRotation(float x, float y, float z);

    const Quaternion& quaternion() const { return quaternion_; }
    void setQuaternion(const Quaternion& q);

    const Vector3& scale() const { return scale_; }
    void setScale(const Vector3& scale);
    void setScale(float x, float y, float z);
    void setScale(float s);

    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    void add(std::shared_ptr<Object3D> child);
    void remove(std::shared_ptr<Object3D> child);
    const std::vector<std::shared_ptr<Object3D>>& children() const { return children_; }
    std::shared_ptr<Object3D> parent() const { return parent_.lock(); }

    std::shared_ptr<Object3D> getObjectByName(const std::string& name);
    void lookAt(const Vector3& target);
    void lookAt(float x, float y, float z);

protected:
    std::string name_;
    Vector3 position_{0, 0, 0};
    Euler rotation_{0, 0, 0};
    Quaternion quaternion_{0, 0, 0, 1};
    Vector3 scale_{1, 1, 1};
    bool visible_ = true;
    std::vector<std::shared_ptr<Object3D>> children_;
    std::weak_ptr<Object3D> parent_;
};

enum class GeometryType { Box, Sphere, Plane, Custom };

class Geometry {
public:
    struct Impl;

    Geometry(GeometryType type = GeometryType::Custom);
    ~Geometry();

    static std::shared_ptr<Geometry> createBox(float width = 1.0f, float height = 1.0f, float depth = 1.0f);
    static std::shared_ptr<Geometry> createSphere(float radius = 1.0f, int widthSegments = 32, int heightSegments = 16);
    static std::shared_ptr<Geometry> createPlane(float width = 1.0f, float height = 1.0f);

    GeometryType geometryType() const { return type_; }

    void setPositions(const std::vector<float>& positions);
    void setNormals(const std::vector<float>& normals);
    void setUVs(const std::vector<float>& uvs);
    void setIndices(const std::vector<uint32_t>& indices);

    const std::vector<float>& positions() const;
    const std::vector<float>& normals() const;
    const std::vector<float>& uvs() const;
    const std::vector<uint32_t>& indices() const;

    Impl* getImpl() const { return impl_.get(); }

private:
    GeometryType type_;
    std::unique_ptr<Impl> impl_;
};

class Material {
public:
    Material();
    ~Material();

    const Color& color() const { return color_; }
    void setColor(const Color& c) { color_ = c; }
    void setColor(float r, float g, float b) { color_ = Color(r, g, b); }
    void setColor(uint32_t hex) { color_ = Color(hex); }

    float roughness() const { return roughness_; }
    void setRoughness(float r) { roughness_ = r; }

    float metalness() const { return metalness_; }
    void setMetalness(float m) { metalness_ = m; }

    bool wireframe() const { return wireframe_; }
    void setWireframe(bool w) { wireframe_ = w; }

    float opacity() const { return opacity_; }
    void setOpacity(float o) { opacity_ = o; }

    bool transparent() const { return transparent_; }
    void setTransparent(bool t) { transparent_ = t; }

    const std::string& map() const { return mapPath_; }
    void setMap(const std::string& path) { mapPath_ = path; }

private:
    Color color_{1.0f, 1.0f, 1.0f, 1.0f};
    float roughness_ = 0.5f;
    float metalness_ = 0.0f;
    bool wireframe_ = false;
    float opacity_ = 1.0f;
    bool transparent_ = false;
    std::string mapPath_;
};

class Mesh : public Object3D {
public:
    Mesh(std::shared_ptr<Geometry> geometry = nullptr, std::shared_ptr<Material> material = nullptr, const std::string& name = "");
    ~Mesh() override;

    ObjectType type() const override { return ObjectType::Mesh; }

    std::shared_ptr<Geometry> geometry() const { return geometry_; }
    void setGeometry(std::shared_ptr<Geometry> geo) { geometry_ = geo; }

    std::shared_ptr<Material> material() const { return material_; }
    void setMaterial(std::shared_ptr<Material> mat) { material_ = mat; }

private:
    std::shared_ptr<Geometry> geometry_;
    std::shared_ptr<Material> material_;
};

class Group : public Object3D {
public:
    explicit Group(const std::string& name = "") : Object3D(name) {}
    ObjectType type() const override { return ObjectType::Group; }
};

class Camera : public Object3D {
public:
    explicit Camera(const std::string& name = "") : Object3D(name) {}
    ObjectType type() const override { return ObjectType::Camera; }

    float nearZ() const { return nearZ_; }
    void setNearZ(float n) { nearZ_ = n; }

    float farZ() const { return farZ_; }
    void setFarZ(float f) { farZ_ = f; }

protected:
    float nearZ_ = 0.1f;
    float farZ_ = 1000.0f;
};

class PerspectiveCamera : public Camera {
public:
    PerspectiveCamera(float fov = 60.0f, float aspect = 1.0f, float nearZ = 0.1f, float farZ = 1000.0f, const std::string& name = "");

    ObjectType type() const override { return ObjectType::PerspectiveCamera; }

    float fov() const { return fov_; }
    void setFov(float fov) { fov_ = fov; }

    float aspect() const { return aspect_; }
    void setAspect(float aspect) { aspect_ = aspect; }

private:
    float fov_ = 60.0f;
    float aspect_ = 1.0f;
};

class Light : public Object3D {
public:
    explicit Light(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "");
    ObjectType type() const override { return ObjectType::Light; }

    const Color& color() const { return color_; }
    void setColor(const Color& c) { color_ = c; }
    void setColor(float r, float g, float b) { color_ = Color(r, g, b); }

    float intensity() const { return intensity_; }
    void setIntensity(float i) { intensity_ = i; }

protected:
    Color color_{1.0f, 1.0f, 1.0f, 1.0f};
    float intensity_ = 1.0f;
};

class AmbientLight : public Light {
public:
    explicit AmbientLight(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "");
    ObjectType type() const override { return ObjectType::AmbientLight; }
};

class DirectionalLight : public Light {
public:
    explicit DirectionalLight(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "");
    ObjectType type() const override { return ObjectType::DirectionalLight; }

    bool castsShadow() const { return castsShadow_; }
    void setCastsShadow(bool shadow) { castsShadow_ = shadow; }

    const Vector3& target() const { return target_; }
    void setTarget(const Vector3& t) { target_ = t; }

private:
    bool castsShadow_ = false;
    Vector3 target_{0, 0, 0};
};

class Scene : public Object3D {
public:
    struct Impl;

    explicit Scene(const std::string& name = "");
    ~Scene() override;

    ObjectType type() const override { return ObjectType::Scene; }

    const Color& background() const { return background_; }
    void setBackground(const Color& color) { background_ = color; }
    void setBackground(float r, float g, float b) { background_ = Color(r, g, b); }

    Impl* getImpl() const { return impl_.get(); }

private:
    Color background_{0.1f, 0.1f, 0.1f, 1.0f};
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// Render & Window Management
// -----------------------------------------------------------------------------

class WebGLRenderer {
public:
    struct Impl;

    WebGLRenderer(int width = 800, int height = 600);
    ~WebGLRenderer();

    void setSize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    void setClearColor(const Color& color, float alpha = 1.0f);
    const Color& clearColor() const { return clearColor_; }

    void render(Scene& scene, Camera& camera);
    void render(std::shared_ptr<Scene> scene, std::shared_ptr<Camera> camera);

    Impl* getImpl() const { return impl_.get(); }

private:
    int width_ = 800;
    int height_ = 600;
    Color clearColor_{0.0f, 0.0f, 0.0f, 1.0f};
    std::unique_ptr<Impl> impl_;
};

void set_render_callback(std::function<void(double dt)> callback);
std::function<void(double dt)> get_render_callback();
void tick_render_callback(double dt);

// -----------------------------------------------------------------------------
// Physics Types
// -----------------------------------------------------------------------------

enum class RigidBodyType { Static, Dynamic, Kinematic };

class RigidBody : public std::enable_shared_from_this<RigidBody> {
public:
    struct Impl;

    RigidBody(RigidBodyType bodyType = RigidBodyType::Dynamic, float mass = 1.0f);
    ~RigidBody();

    RigidBodyType bodyType() const { return bodyType_; }
    float mass() const { return mass_; }

    Vector3 position() const;
    void setPosition(const Vector3& pos);

    Quaternion rotation() const;
    void setRotation(const Quaternion& rot);

    Vector3 linearVelocity() const;
    void setLinearVelocity(const Vector3& vel);

    Vector3 angularVelocity() const;
    void setAngularVelocity(const Vector3& vel);

    void addForce(const Vector3& force);
    void addImpulse(const Vector3& impulse);

    float friction() const { return friction_; }
    void setFriction(float f);

    float restitution() const { return restitution_; }
    void setRestitution(float r);

    Impl* getImpl() const { return impl_.get(); }

private:
    RigidBodyType bodyType_ = RigidBodyType::Dynamic;
    float mass_ = 1.0f;
    float friction_ = 0.5f;
    float restitution_ = 0.0f;
    std::unique_ptr<Impl> impl_;
};

struct RaycastHit {
    bool hit = false;
    Vector3 point{0, 0, 0};
    Vector3 normal{0, 1, 0};
    float distance = 0.0f;
    std::shared_ptr<RigidBody> body = nullptr;
};

class PhysicsWorld {
public:
    struct Impl;

    PhysicsWorld();
    ~PhysicsWorld();

    void setGravity(const Vector3& gravity);
    Vector3 gravity() const;

    void step(double dt);

    void addBody(std::shared_ptr<RigidBody> body);
    void removeBody(std::shared_ptr<RigidBody> body);
    const std::vector<std::shared_ptr<RigidBody>>& bodies() const;

    RaycastHit raycast(const Vector3& origin, const Vector3& direction, float maxDistance = 1000.0f);

    Impl* getImpl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace bro
