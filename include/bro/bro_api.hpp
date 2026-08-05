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
struct Vector3;
struct Color;
struct Quaternion;
struct Euler;
struct Matrix4;
class Object3D;
class Scene;
class Geometry;
class CylinderGeometry;
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
    Vector3* normalize() {
        float len = length();
        if (len > 0.0f) { x /= len; y /= len; z /= len; }
        return this;
    }
    float dot(const Vector3& rhs) const { return x * rhs.x + y * rhs.y + z * rhs.z; }
    float dot(const Vector3* rhs) const { return rhs ? dot(*rhs) : 0.0f; }
    Vector3 cross(const Vector3& rhs) const {
        return Vector3(
            y * rhs.z - z * rhs.y,
            z * rhs.x - x * rhs.z,
            x * rhs.y - y * rhs.x
        );
    }
    Vector3* cross(const Vector3* rhs) {
        if (rhs) { Vector3 res = cross(*rhs); x = res.x; y = res.y; z = res.z; }
        return this;
    }
    Vector3* set(float px, float py, float pz) { x = px; y = py; z = pz; return this; }
    float distanceTo(const Vector3& v) const { return (*this - v).length(); }
    float distanceTo(const Vector3* v) const { return v ? distanceTo(*v) : 0.0f; }

    Vector3* add(const Vector3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return this; }
    Vector3* add(const Vector3* rhs) { if (rhs) { x += rhs->x; y += rhs->y; z += rhs->z; } return this; }
    Vector3* sub(const Vector3& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return this; }
    Vector3* sub(const Vector3* rhs) { if (rhs) { x -= rhs->x; y -= rhs->y; z -= rhs->z; } return this; }
    Vector3* multiplyScalar(float s) { x *= s; y *= s; z *= s; return this; }
    Vector3* divideScalar(float s) { if (s != 0.0f) { x /= s; y /= s; z /= s; } return this; }
    Vector3* copy(const Vector3& rhs) { x = rhs.x; y = rhs.y; z = rhs.z; return this; }
    Vector3* copy(const Vector3* rhs) { if (rhs) { x = rhs->x; y = rhs->y; z = rhs->z; } return this; }
    Vector3* addVectors(const Vector3& a, const Vector3& b) { x = a.x + b.x; y = a.y + b.y; z = a.z + b.z; return this; }
    Vector3* addVectors(const Vector3* a, const Vector3* b) { if (a && b) { x = a->x + b->x; y = a->y + b->y; z = a->z + b->z; } return this; }
    Vector3* subVectors(const Vector3& a, const Vector3& b) { x = a.x - b.x; y = a.y - b.y; z = a.z - b.z; return this; }
    Vector3* subVectors(const Vector3* a, const Vector3* b) { if (a && b) { x = a->x - b->x; y = a->y - b->y; z = a->z - b->z; } return this; }
    Vector3* lerp(const Vector3& v, float alpha) { x += (v.x - x) * alpha; y += (v.y - y) * alpha; z += (v.z - z) * alpha; return this; }
    Vector3* lerp(const Vector3* v, float alpha) { if (v) lerp(*v, alpha); return this; }
    Vector3* applyMatrix4(const Matrix4& m);
    Vector3* applyMatrix4(const Matrix4* m);
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr Color() = default;
    constexpr Color(float pr, float pg, float pb, float pa = 1.0f) : r(pr), g(pg), b(pb), a(pa) {}
    Color(uint32_t hex, float pa = 1.0f)
        : r(((hex >> 16) & 0xFF) / 255.0f),
          g(((hex >> 8) & 0xFF) / 255.0f),
          b((hex & 0xFF) / 255.0f),
          a(pa) {}

    Color* setRGB(float pr, float pg, float pb) { r = pr; g = pg; b = pb; return this; }
    Color* setHex(uint32_t hex) {
        r = ((hex >> 16) & 0xFF) / 255.0f;
        g = ((hex >> 8) & 0xFF) / 255.0f;
        b = (hex & 0xFF) / 255.0f;
        return this;
    }
    Color* setHSL(float h, float s, float l) {
        if (s == 0.0f) {
            r = g = b = l;
        } else {
            auto hue2rgb = [](float p, float q, float t) {
                if (t < 0.0f) t += 1.0f;
                if (t > 1.0f) t -= 1.0f;
                if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
                if (t < 1.0f / 2.0f) return q;
                if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
                return p;
            };
            float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
            float p = 2.0f * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3.0f);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3.0f);
        }
        return this;
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

    Quaternion* set(float px, float py, float pz, float pw) { x = px; y = py; z = pz; w = pw; return this; }
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

    Euler* set(float px, float py, float pz, std::string pOrder = "XYZ") {
        x = px; y = py; z = pz; order = std::move(pOrder); return this;
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

    Matrix4* makeRotationX(float theta) {
        float c = std::cos(theta), s = std::sin(theta);
        elements[0] = 1.0f; elements[4] = 0.0f; elements[8] = 0.0f;  elements[12] = 0.0f;
        elements[1] = 0.0f; elements[5] = c;    elements[9] = -s;    elements[13] = 0.0f;
        elements[2] = 0.0f; elements[6] = s;    elements[10] = c;    elements[14] = 0.0f;
        elements[3] = 0.0f; elements[7] = 0.0f; elements[11] = 0.0f; elements[15] = 1.0f;
        return this;
    }

    Matrix4* makeRotationY(float theta) {
        float c = std::cos(theta), s = std::sin(theta);
        elements[0] = c;    elements[4] = 0.0f; elements[8] = s;     elements[12] = 0.0f;
        elements[1] = 0.0f; elements[5] = 1.0f; elements[9] = 0.0f;  elements[13] = 0.0f;
        elements[2] = -s;   elements[6] = 0.0f; elements[10] = c;    elements[14] = 0.0f;
        elements[3] = 0.0f; elements[7] = 0.0f; elements[11] = 0.0f; elements[15] = 1.0f;
        return this;
    }

    Matrix4* makeRotationZ(float theta) {
        float c = std::cos(theta), s = std::sin(theta);
        elements[0] = c;    elements[4] = -s;   elements[8] = 0.0f;  elements[12] = 0.0f;
        elements[1] = s;    elements[5] = c;    elements[9] = 0.0f;  elements[13] = 0.0f;
        elements[2] = 0.0f; elements[6] = 0.0f; elements[10] = 1.0f; elements[14] = 0.0f;
        elements[3] = 0.0f; elements[7] = 0.0f; elements[11] = 0.0f; elements[15] = 1.0f;
        return this;
    }

    Matrix4* copy(const Matrix4& m) {
        elements = m.elements;
        return this;
    }
    Matrix4* copy(const Matrix4* m) {
        if (m) elements = m->elements;
        return this;
    }

    Matrix4* multiply(const Matrix4& m) {
        return multiplyMatrices(*this, m);
    }
    Matrix4* multiply(const Matrix4* m) {
        if (m) multiply(*m);
        return this;
    }

    Matrix4* multiplyMatrices(const Matrix4& a, const Matrix4& b) {
        const float* ae = a.elements.data();
        const float* be = b.elements.data();
        float a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
        float a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
        float a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
        float a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];
        float b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
        float b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
        float b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
        float b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];

        elements[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
        elements[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
        elements[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
        elements[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;

        elements[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
        elements[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
        elements[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
        elements[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;

        elements[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
        elements[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
        elements[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
        elements[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;

        elements[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
        elements[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
        elements[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
        elements[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
        return this;
    }
    Matrix4* multiplyMatrices(const Matrix4* a, const Matrix4* b) {
        if (a && b) multiplyMatrices(*a, *b);
        return this;
    }

    Matrix4* getInverse(const Matrix4& m) {
        const float* me = m.elements.data();
        float n11 = me[0], n21 = me[1], n31 = me[2], n41 = me[3];
        float n12 = me[4], n22 = me[5], n32 = me[6], n42 = me[7];
        float n13 = me[8], n23 = me[9], n33 = me[10], n43 = me[11];
        float n14 = me[12], n24 = me[13], n34 = me[14], n44 = me[15];

        float t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 - n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
        float t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 + n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
        float t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 - n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
        float t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;

        float det = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
        if (det == 0.0f) return this;
        float detInv = 1.0f / det;

        elements[0] = t11 * detInv;
        elements[1] = (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 + n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * detInv;
        elements[2] = (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 - n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * detInv;
        elements[3] = (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 + n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * detInv;

        elements[4] = t12 * detInv;
        elements[5] = (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 - n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * detInv;
        elements[6] = (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 + n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * detInv;
        elements[7] = (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 - n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * detInv;

        elements[8] = t13 * detInv;
        elements[9] = (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 + n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * detInv;
        elements[10] = (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 - n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * detInv;
        elements[11] = (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 + n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * detInv;

        elements[12] = t14 * detInv;
        elements[13] = (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 - n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * detInv;
        elements[14] = (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 + n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * detInv;
        elements[15] = (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 - n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * detInv;
        return this;
    }
    Matrix4* getInverse(const Matrix4* m) {
        if (m) return getInverse(*m);
        return this;
    }
};

inline Vector3* Vector3::applyMatrix4(const Matrix4& m) {
    float x_ = x, y_ = y, z_ = z;
    const float* e = m.elements.data();
    float w = 1.0f / (e[3] * x_ + e[7] * y_ + e[11] * z_ + e[15]);

    x = (e[0] * x_ + e[4] * y_ + e[8] * z_ + e[12]) * w;
    y = (e[1] * x_ + e[5] * y_ + e[9] * z_ + e[13]) * w;
    z = (e[2] * x_ + e[6] * y_ + e[10] * z_ + e[14]) * w;
    return this;
}

inline Vector3* Vector3::applyMatrix4(const Matrix4* m) {
    if (m) applyMatrix4(*m);
    return this;
}

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
    Object3D(const std::string& name = "") : name_(name) {}
    virtual ~Object3D() = default;

    virtual ObjectType type() const { return ObjectType::Object3D; }

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    Vector3& position() { return position_; }
    const Vector3& position() const { return position_; }
    void setPosition(const Vector3& pos) { position_ = pos; }
    void setPosition(float x, float y, float z) { position_ = Vector3(x, y, z); }

    Euler& rotation() { return rotation_; }
    const Euler& rotation() const { return rotation_; }
    void setRotation(const Euler& rot) { rotation_ = rot; quaternion_ = rot.toQuaternion(); }
    void setRotation(float x, float y, float z) { rotation_ = Euler(x, y, z); quaternion_ = rotation_.toQuaternion(); }

    Quaternion& quaternion() { return quaternion_; }
    const Quaternion& quaternion() const { return quaternion_; }
    void setQuaternion(const Quaternion& q) { quaternion_ = q; }

    Vector3& scale() { return scale_; }
    const Vector3& scale() const { return scale_; }
    void setScale(const Vector3& scale) { scale_ = scale; }
    void setScale(float x, float y, float z) { scale_ = Vector3(x, y, z); }
    void setScale(float s) { scale_ = Vector3(s, s, s); }

    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    void add(std::shared_ptr<Object3D> child) { if (child) { children_.push_back(child); } }
    void add(Object3D* child) { if (child) add(std::shared_ptr<Object3D>(child)); }
    void remove(std::shared_ptr<Object3D> child) {}
    void remove(Object3D* child) { if (child) remove(std::shared_ptr<Object3D>(child)); }
    const std::vector<std::shared_ptr<Object3D>>& children() const { return children_; }
    std::shared_ptr<Object3D> parent() const { return parent_.lock(); }

    std::shared_ptr<Object3D> getObjectByName(const std::string& name) { return nullptr; }
    void lookAt(const Vector3& target) {}
    void lookAt(float x, float y, float z) {}

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
    struct Impl {};

    Geometry(GeometryType type = GeometryType::Custom) : type_(type) {}
    virtual ~Geometry() = default;

    static std::shared_ptr<Geometry> createBox(float width = 1.0f, float height = 1.0f, float depth = 1.0f) {
        auto geo = std::make_shared<Geometry>(GeometryType::Box);
        float hw = width * 0.5f, hh = height * 0.5f, hd = depth * 0.5f;
        geo->positions_ = {
            -hw, -hh,  hd,  hw, -hh,  hd,  hw,  hh,  hd, -hw,  hh,  hd,
            -hw, -hh, -hd, -hw,  hh, -hd,  hw,  hh, -hd,  hw, -hh, -hd,
            -hw,  hh, -hd, -hw,  hh,  hd,  hw,  hh,  hd,  hw,  hh, -hd,
            -hw, -hh, -hd,  hw, -hh, -hd,  hw, -hh,  hd, -hw, -hh,  hd,
             hw, -hh, -hd,  hw,  hh, -hd,  hw,  hh,  hd,  hw, -hh,  hd,
            -hw, -hh, -hd, -hw, -hh,  hd, -hw,  hh,  hd, -hw,  hh, -hd
        };
        geo->normals_ = {
             0,  0,  1,   0,  0,  1,   0,  0,  1,   0,  0,  1,
             0,  0, -1,   0,  0, -1,   0,  0, -1,   0,  0, -1,
             0,  1,  0,   0,  1,  0,   0,  1,  0,   0,  1,  0,
             0, -1,  0,   0, -1,  0,   0, -1,  0,   0, -1,  0,
             1,  0,  0,   1,  0,  0,   1,  0,  0,   1,  0,  0,
            -1,  0,  0,  -1,  0,  0,  -1,  0,  0,  -1,  0,  0
        };
        geo->indices_ = {
             0,  1,  2,   0,  2,  3,
             4,  5,  6,   4,  6,  7,
             8,  9, 10,   8, 10, 11,
            12, 13, 14,  12, 14, 15,
            16, 17, 18,  16, 18, 19,
            20, 21, 22,  20, 22, 23
        };
        return geo;
    }

    static std::shared_ptr<Geometry> createSphere(float radius = 1.0f, int widthSegments = 32, int heightSegments = 16) {
        auto geo = std::make_shared<Geometry>(GeometryType::Sphere);
        for (int y = 0; y <= heightSegments; ++y) {
            float v = static_cast<float>(y) / heightSegments;
            float phi = v * 3.14159265f;
            for (int x = 0; x <= widthSegments; ++x) {
                float u = static_cast<float>(x) / widthSegments;
                float theta = u * 6.2831853f;
                float px = -radius * std::cos(theta) * std::sin(phi);
                float py = radius * std::cos(phi);
                float pz = radius * std::sin(theta) * std::sin(phi);
                geo->positions_.push_back(px);
                geo->positions_.push_back(py);
                geo->positions_.push_back(pz);
                float len = std::sqrt(px * px + py * py + pz * pz);
                geo->normals_.push_back(len > 0 ? px / len : 0);
                geo->normals_.push_back(len > 0 ? py / len : 1);
                geo->normals_.push_back(len > 0 ? pz / len : 0);
                geo->uvs_.push_back(u);
                geo->uvs_.push_back(1.0f - v);
            }
        }
        for (int y = 0; y < heightSegments; ++y) {
            for (int x = 0; x < widthSegments; ++x) {
                uint32_t first = y * (widthSegments + 1) + x;
                uint32_t second = first + widthSegments + 1;
                geo->indices_.push_back(first);
                geo->indices_.push_back(second);
                geo->indices_.push_back(first + 1);
                geo->indices_.push_back(second);
                geo->indices_.push_back(second + 1);
                geo->indices_.push_back(first + 1);
            }
        }
        return geo;
    }

    static std::shared_ptr<Geometry> createPlane(float width = 1.0f, float height = 1.0f) {
        auto geo = std::make_shared<Geometry>(GeometryType::Plane);
        float hw = width * 0.5f, hh = height * 0.5f;
        geo->positions_ = {-hw, -hh, 0.0f, hw, -hh, 0.0f, hw, hh, 0.0f, -hw, hh, 0.0f};
        geo->normals_ = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        geo->uvs_ = {0, 0, 1, 0, 1, 1, 0, 1};
        geo->indices_ = {0, 1, 2, 0, 2, 3};
        return geo;
    }

    GeometryType geometryType() const { return type_; }

    const std::vector<float>& positions() const { return positions_; }
    void setPositions(const std::vector<float>& p) { positions_ = p; }

    const std::vector<float>& normals() const { return normals_; }
    void setNormals(const std::vector<float>& n) { normals_ = n; }

    const std::vector<float>& uvs() const { return uvs_; }
    void setUVs(const std::vector<float>& u) { uvs_ = u; }

    const std::vector<uint32_t>& indices() const { return indices_; }
    void setIndices(const std::vector<uint32_t>& i) { indices_ = i; }

    Impl* getImpl() const { return impl_.get(); }

private:
    GeometryType type_;
    std::vector<float> positions_;
    std::vector<float> normals_;
    std::vector<float> uvs_;
    std::vector<uint32_t> indices_;
    std::unique_ptr<Impl> impl_;
};

class CylinderGeometry : public Geometry {
public:
    CylinderGeometry(float radiusTop = 1.0f, float radiusBottom = 1.0f, float height = 1.0f, int radialSegments = 8, int heightSegments = 1)
        : Geometry(GeometryType::Custom) {}
};

class Material {
public:
    Material() = default;
    virtual ~Material() = default;

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
    Mesh(std::shared_ptr<Geometry> geometry = nullptr, std::shared_ptr<Material> material = nullptr, const std::string& name = "")
        : Object3D(name), geometry_(geometry), material_(material) {}
    Mesh(Geometry* geo, Material* mat = nullptr, const std::string& name = "")
        : Object3D(name), geometry_(geo, [](Geometry*){}), material_(mat, [](Material*){}) {}
    ~Mesh() override = default;

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
    PerspectiveCamera(float fov = 60.0f, float aspect = 1.0f, float nearZ = 0.1f, float farZ = 1000.0f, const std::string& name = "")
        : Camera(name), fov(fov), aspect(aspect) { nearZ_ = nearZ; farZ_ = farZ; }
    ~PerspectiveCamera() override = default;

    ObjectType type() const override { return ObjectType::PerspectiveCamera; }

    void updateProjectionMatrix() {}

    float fov = 60.0f;
    float aspect = 1.0f;
};

class Light : public Object3D {
public:
    explicit Light(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "")
        : Object3D(name), color_(color), intensity_(intensity) {}
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
    explicit AmbientLight(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "")
        : Light(color, intensity, name) {}
    ObjectType type() const override { return ObjectType::AmbientLight; }
};

class DirectionalLight : public Light {
public:
    explicit DirectionalLight(const Color& color = Color(1, 1, 1), float intensity = 1.0f, const std::string& name = "")
        : Light(color, intensity, name) {}
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
    void render(Scene* scene, Camera* camera) { if (scene && camera) render(*scene, *camera); }

    void* domElement = nullptr;
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
    void addBody(RigidBody* body) { if (body) addBody(std::shared_ptr<RigidBody>(body)); }
    void removeBody(std::shared_ptr<RigidBody> body);
    void removeBody(RigidBody* body) { if (body) removeBody(std::shared_ptr<RigidBody>(body)); }

    const std::vector<std::shared_ptr<RigidBody>>& bodies() const;

    RaycastHit raycast(const Vector3& origin, const Vector3& direction, float maxDistance = 1000.0f);

    Impl* getImpl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

class Clock {
public:
    double getElapsedTime() { static double t = 0.0; t += 0.016; return t; }
};

} // namespace bro
