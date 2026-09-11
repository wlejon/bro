#include "engine/gizmo.h"
#include "engine/gizmo_internal.h"

#include "scene/mesh_node.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

using scene::MeshNode;
using bromath::Quat;
using bromath::Vec3;

GizmoManager::GizmoManager() = default;

GizmoManager::~GizmoManager() {
    clearCallbacks();
}

void GizmoManager::clearCallbacks() {
    onGetPosition = nullptr;
    onGetOrientation = nullptr;
    onTranslate = nullptr;
    onRotate = nullptr;
    onScale = nullptr;
    onBeginDrag = nullptr;
    onEndDrag = nullptr;
    onHoverChange = nullptr;
}

void GizmoManager::setPosition(float x, float y, float z) {
    position_.x = x; position_.y = y; position_.z = z;
}

void GizmoManager::setHovered(GizmoAxis axis) {
    if (hovered_ == axis) return;
    hovered_ = axis;

    auto apply = [&](MeshNode* n, const float (&base)[4], GizmoAxis axisId) {
        if (!n) return;
        if (hovered_ == axisId || (isDragging() && dragAxis_ == axisId)) {
            n->setColor(config_.colorHover[0], config_.colorHover[1],
                        config_.colorHover[2], config_.colorHover[3]);
        } else {
            n->setColor(base[0], base[1], base[2], base[3]);
        }
    };
    apply(arrowX_.get(), config_.colorX, GizmoAxis::X);
    apply(arrowY_.get(), config_.colorY, GizmoAxis::Y);
    apply(arrowZ_.get(), config_.colorZ, GizmoAxis::Z);
    apply(ringX_.get(),  config_.colorX, GizmoAxis::X);
    apply(ringY_.get(),  config_.colorY, GizmoAxis::Y);
    apply(ringZ_.get(),  config_.colorZ, GizmoAxis::Z);
    apply(scaleX_.get(), config_.colorX, GizmoAxis::X);
    apply(scaleY_.get(), config_.colorY, GizmoAxis::Y);
    apply(scaleZ_.get(), config_.colorZ, GizmoAxis::Z);

    const float planeZ[4] = { config_.colorZ[0], config_.colorZ[1], config_.colorZ[2], kGizmoPlaneAlpha };
    const float planeX[4] = { config_.colorX[0], config_.colorX[1], config_.colorX[2], kGizmoPlaneAlpha };
    const float planeY[4] = { config_.colorY[0], config_.colorY[1], config_.colorY[2], kGizmoPlaneAlpha };
    static const float kViewBase[4] = { 0.85f, 0.85f, 0.85f, 0.9f };
    apply(planeXY_.get(), planeZ, GizmoAxis::XY);
    apply(planeYZ_.get(), planeX, GizmoAxis::YZ);
    apply(planeXZ_.get(), planeY, GizmoAxis::XZ);
    apply(ringView_.get(), kViewBase, GizmoAxis::View);

    fireHoverChange();
}

void GizmoManager::refreshFromCallbacks() {
    if (onGetPosition) {
        position_ = onGetPosition();
    }
    if (config_.space == GizmoSpace::Local && onGetOrientation) {
        orientation_ = onGetOrientation();
    }
}

void GizmoManager::fireTranslate(const Vec3& d) {
    if (onTranslate) onTranslate(d);
}

void GizmoManager::fireRotate(const Quat& q) {
    if (onRotate) onRotate(q);
}

void GizmoManager::fireScale(const Vec3& s) {
    if (onScale) onScale(s);
}

void GizmoManager::fireBegin() {
    if (onBeginDrag) onBeginDrag();
}

void GizmoManager::fireEnd(bool committed) {
    if (onEndDrag) onEndDrag(committed);
}

void GizmoManager::fireHoverChange() {
    if (onHoverChange) onHoverChange();
}

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------

GizmoManager::RaySegResult
GizmoManager::closestRayToSegment(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& A, const Vec3& B) {
    Vec3 u = B - A;
    Vec3 w = rayO - A;
    float a = rayD.x*rayD.x + rayD.y*rayD.y + rayD.z*rayD.z;
    float b = rayD.x*u.x + rayD.y*u.y + rayD.z*u.z;
    float c = u.x*u.x + u.y*u.y + u.z*u.z;
    float d = rayD.x*w.x + rayD.y*w.y + rayD.z*w.z;
    float e = u.x*w.x + u.y*w.y + u.z*w.z;
    float denom = a*c - b*b;
    float rayT, segT;
    if (std::fabs(denom) < 1e-9f) {
        segT = 0.5f;
        rayT = (b * segT - d) / (a != 0 ? a : 1.0f);
    } else {
        rayT = (b*e - c*d) / denom;
        segT = (a*e - b*d) / denom;
        segT = clamp01(segT);
        rayT = (b * segT - d) / (a != 0 ? a : 1.0f);
    }
    Vec3 segP = A + u * segT;
    Vec3 rayP = rayO + rayD * rayT;
    Vec3 diff = segP - rayP;
    float dist = vlen_(diff);
    return { rayT, segT, dist, segP };
}

bool GizmoManager::rayVsAxisParam(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& pivot, const Vec3& axisDir,
                                  float& outParam) {
    Vec3 w = rayO - pivot;
    Vec3 u = axisDir;
    float a = rayD.x*rayD.x + rayD.y*rayD.y + rayD.z*rayD.z;
    float b = rayD.x*u.x + rayD.y*u.y + rayD.z*u.z;
    float c = u.x*u.x + u.y*u.y + u.z*u.z;
    float d = rayD.x*w.x + rayD.y*w.y + rayD.z*w.z;
    float e = u.x*w.x + u.y*w.y + u.z*w.z;
    float denom = a*c - b*b;
    if (!std::isfinite(denom) || std::fabs(denom) < 1e-7f * (a * c)) return false;
    outParam = (a*e - b*d) / denom;
    return std::isfinite(outParam);
}

bool GizmoManager::rayVsPlane(const Vec3& rayO, const Vec3& rayD,
                              const Vec3& pivot, const Vec3& normal,
                              Vec3& outPoint) {
    float denom = rayD.x*normal.x + rayD.y*normal.y + rayD.z*normal.z;
    if (std::fabs(denom) < 1e-6f) return false;
    Vec3 diff = pivot - rayO;
    float t = (diff.x*normal.x + diff.y*normal.y + diff.z*normal.z) / denom;
    if (t < 0) return false;
    outPoint = rayO + rayD * t;
    return true;
}

Quat GizmoManager::quatAxisAngle(const Vec3& axis, float radians) {
    return bromath::qaxisAngle(vnorm_(axis), radians);
}

bool GizmoManager::viewPlanePoint(const Vec3& rayO, const Vec3& rayD,
                                  const Vec3& pivot, const Vec3& planeNormal,
                                  Vec3& outPoint) const {
    return rayVsPlane(rayO, rayD, pivot, planeNormal, outPoint);
}

bool GizmoManager::worldToScreen(const CameraSnapshot& cam, const Vec3& p,
                                 float& outX, float& outY) {
    if (!cam.valid || cam.canvasW <= 0 || cam.canvasH <= 0) return false;
    Vec3 v = p - cam.eye;
    float xc = v.x*cam.right.x   + v.y*cam.right.y   + v.z*cam.right.z;
    float yc = v.x*cam.up.x      + v.y*cam.up.y      + v.z*cam.up.z;
    float zc = v.x*cam.forward.x + v.y*cam.forward.y + v.z*cam.forward.z;

    float nx, ny;
    if (cam.perspective) {
        if (zc < 1e-5f) return false;
        float tanHalfFov = 1.0f / cam.p11;
        nx = xc / (zc * cam.aspect * tanHalfFov);
        ny = yc / (zc * tanHalfFov);
    } else {
        nx = xc * cam.p00;
        ny = yc * cam.p11;
    }
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
    outX = (nx + 1.0f) * 0.5f * static_cast<float>(cam.canvasW);
    outY = (1.0f - ny) * 0.5f * static_cast<float>(cam.canvasH);
    return true;
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

GizmoManager::PickResult
GizmoManager::pick(const Vec3& rayO, const Vec3& rayD) {
    PickResult out;
    if (!config_.visible) return out;

    Vec3 axX, axY, axZ;
    resolveAxes(axX, axY, axZ);
    Vec3 axes[3] = { axX, axY, axZ };
    GizmoAxis axisIds[3] = { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };

    float bestDist = 1e30f;
    float bestT    = 1e30f;
    auto consider = [&](float dist, float rayT, GizmoAxis axis,
                        const Vec3& axisDir, const Vec3& hitPoint) {
        if (dist > bestDist + 1e-6f) return;
        if (std::fabs(dist - bestDist) <= 1e-6f && rayT >= bestT) return;
        bestDist = dist;
        bestT    = rayT;
        out.axis = axis;
        out.axisDir = axisDir;
        out.rayT = rayT;
        out.hitPoint = hitPoint;
    };

    switch (config_.mode) {
    case GizmoMode::Translate: {
        float armLen  = arrow_.length() * currentScale_;
        float pickRad = arrow_.tipRadius * currentScale_ * 2.4f;
        for (int i = 0; i < 3; ++i) {
            Vec3 tip = position_ + axes[i] * armLen;
            auto r = closestRayToSegment(rayO, rayD, position_, tip);
            if (r.rayT < 0) continue;
            if (r.dist > pickRad) continue;
            consider(r.dist, r.rayT, axisIds[i], axes[i], r.segPoint);
        }
        {
            const GizmoAxis planeIds[3] = { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ };
            const float lo = plane_.offset * currentScale_;
            const float hi = plane_.outer() * currentScale_;
            for (GizmoAxis pid : planeIds) {
                Vec3 u, v, n;
                if (!planeBasis(pid, axX, axY, axZ, u, v, n)) continue;
                Vec3 hit;
                if (!rayVsPlane(rayO, rayD, position_, n, hit)) continue;
                Vec3 rel = hit - position_;
                float du = rel.x*u.x + rel.y*u.y + rel.z*u.z;
                float dv = rel.x*v.x + rel.y*v.y + rel.z*v.z;
                if (du < lo || du > hi || dv < lo || dv > hi) continue;
                float t = (hit - rayO).x*rayD.x + (hit - rayO).y*rayD.y + (hit - rayO).z*rayD.z;
                if (t < 0) continue;
                consider(0.0f, t, pid, n, hit);
            }
        }
        break;
    }
    case GizmoMode::Scale: {
        float armLen  = scaleGeom_.length() * currentScale_;
        float pickRad = scaleGeom_.cubeSize * 0.7f * currentScale_;
        for (int i = 0; i < 3; ++i) {
            Vec3 tip = position_ + axes[i] * armLen;
            auto r = closestRayToSegment(rayO, rayD, position_, tip);
            if (r.rayT < 0) continue;
            if (r.dist > pickRad) continue;
            consider(r.dist, r.rayT, axisIds[i], axes[i], r.segPoint);
        }
        {
            float rad = scaleGeom_.cubeSize * 0.9f * currentScale_;
            Vec3 diff = position_ - rayO;
            float tProj = diff.x*rayD.x + diff.y*rayD.y + diff.z*rayD.z;
            if (tProj > 0) {
                Vec3 closest = rayO + rayD * tProj;
                Vec3 dd = closest - position_;
                float d = vlen_(dd);
                if (d < rad)
                    consider(d, tProj, GizmoAxis::Center, Vec3(1, 1, 1), closest);
            }
        }
        break;
    }
    case GizmoMode::Rotate: {
        float majorR = ring_.majorRadius * currentScale_;
        float tube   = ring_.tubeRadius  * currentScale_ * 3.0f;
        for (int i = 0; i < 3; ++i) {
            Vec3 normal = axes[i];
            Vec3 hit;
            if (!rayVsPlane(rayO, rayD, position_, normal, hit)) continue;
            Vec3 rel = hit - position_;
            float r = vlen_(rel);
            float bandDist = std::fabs(r - majorR);
            if (bandDist > tube) continue;
            float t = (hit - rayO).x * rayD.x + (hit - rayO).y * rayD.y + (hit - rayO).z * rayD.z;
            if (t < 0) continue;
            consider(bandDist, t, axisIds[i], normal, hit);
        }
        {
            float viewR = majorR * kGizmoViewRingScale;
            Vec3 hit;
            if (rayVsPlane(rayO, rayD, position_, viewDir_, hit)) {
                Vec3 rel = hit - position_;
                float bandDist = std::fabs(vlen_(rel) - viewR);
                float t = (hit - rayO).x*rayD.x + (hit - rayO).y*rayD.y + (hit - rayO).z*rayD.z;
                if (bandDist <= tube && t >= 0)
                    consider(bandDist, t, GizmoAxis::View, viewDir_, hit);
            }
        }
        break;
    }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Drag lifecycle
// ---------------------------------------------------------------------------

void GizmoManager::beginDrag(const PickResult& hit,
                             const Vec3& rayO, const Vec3& rayD,
                             float screenX, float screenY) {
    if (hit.axis == GizmoAxis::None) return;
    dragAxis_    = hit.axis;
    dragPivot_   = position_;
    dragAxisDir_ = hit.axisDir;
    dragLastPoint_ = hit.hitPoint;
    dragLastScale_ = Vec3(1, 1, 1);

    if (isPlaneAxis(dragAxis_)) {
        dragNormal_ = hit.axisDir;
        dragLastPoint_ = hit.hitPoint;
        setHovered(dragAxis_);
        fireBegin();
        return;
    }

    switch (config_.mode) {
    case GizmoMode::Translate:
    case GizmoMode::Scale: {
        dragParamValid_ = rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_,
                                         dragRefParam_);
        if (!dragParamValid_) dragRefParam_ = 0.0f;
        dragLastParam_ = dragRefParam_;
        break;
    }
    case GizmoMode::Rotate: {
        dragNormal_ = hit.axisDir;
        dragViewDir_ = viewDir_;
        dragCam_ = cam_;

        Vec3 rel = hit.hitPoint - dragPivot_;
        Vec3 worldTangent = bromath::vcross(dragNormal_, rel);

        const float kProbe = 0.05f;
        float px_, py_, rx_, ry_, gx, gy, tx, ty;
        float radius = ring_.majorRadius * currentScale_ *
                       (hit.axis == GizmoAxis::View ? kGizmoViewRingScale : 1.0f);
        dragParamValid_ =
            radius > 1e-5f && vlen_(rel) > 1e-5f && vlen_(worldTangent) > 1e-5f &&
            worldToScreen(dragCam_, hit.hitPoint, gx, gy) &&
            worldToScreen(dragCam_, hit.hitPoint + vnorm_(worldTangent) * (radius * kProbe),
                          tx, ty) &&
            worldToScreen(dragCam_, dragPivot_, px_, py_) &&
            worldToScreen(dragCam_, dragPivot_ + dragCam_.right * radius, rx_, ry_);

        if (dragParamValid_) {
            float dx = tx - gx, dy = ty - gy;
            float dl = std::sqrt(dx*dx + dy*dy);
            float ex = rx_ - px_, ey = ry_ - py_;
            float ppr = std::sqrt(ex*ex + ey*ey);
            if (ppr > 1e-3f && dl > 1e-6f) {
                dragRadius_ = ppr;
                dragTangent_ = Vec3(dx / dl, dy / dl, 0.0f);
                dragLastPoint_ = Vec3(screenX, screenY, 0.0f);
            } else {
                dragParamValid_ = false;
            }
        }
        if (!dragParamValid_) {
            dragTangent_ = Vec3(0, 0, 0);
            dragRadius_ = 1.0f;
        }
        break;
    }
    }
    setHovered(dragAxis_);
    fireBegin();
}

bool GizmoManager::updateDrag(const Vec3& rayO, const Vec3& rayD,
                              float screenX, float screenY,
                              Vec3& outTranslate, Quat& outRotate, Vec3& outScale) {
    outTranslate = Vec3(0, 0, 0);
    outRotate    = bromath::qidentity();
    outScale     = Vec3(1, 1, 1);
    if (!isDragging()) return false;

    if (isPlaneAxis(dragAxis_)) {
        Vec3 hit;
        if (!rayVsPlane(rayO, rayD, dragPivot_, dragNormal_, hit)) return true;
        outTranslate = hit - dragLastPoint_;
        dragLastPoint_ = hit;
        fireTranslate(outTranslate);
        return true;
    }

    switch (config_.mode) {
    case GizmoMode::Translate: {
        float t;
        if (!rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_, t))
            return true;
        if (!dragParamValid_) {
            dragParamValid_ = true;
            dragRefParam_ = t;
            dragLastParam_ = t;
            return true;
        }
        float dt = t - dragLastParam_;
        dragLastParam_ = t;
        outTranslate = dragAxisDir_ * dt;
        break;
    }
    case GizmoMode::Scale: {
        float t;
        if (!rayVsAxisParam(rayO, rayD, dragPivot_, dragAxisDir_, t))
            return true;
        if (!dragParamValid_) {
            dragParamValid_ = true;
            dragRefParam_ = t;
            dragLastParam_ = t;
            return true;
        }
        float ref = dragRefParam_;
        if (std::fabs(ref) < 1e-4f) ref = (ref < 0 ? -1e-4f : 1e-4f);
        float factor = t / ref;
        if (dragAxis_ == GizmoAxis::Center) {
            outScale = Vec3(factor, factor, factor);
        } else if (dragAxis_ == GizmoAxis::X) {
            outScale = Vec3(factor, 1, 1);
        } else if (dragAxis_ == GizmoAxis::Y) {
            outScale = Vec3(1, factor, 1);
        } else if (dragAxis_ == GizmoAxis::Z) {
            outScale = Vec3(1, 1, factor);
        }
        Vec3 delta(outScale.x / dragLastScale_.x,
                   outScale.y / dragLastScale_.y,
                   outScale.z / dragLastScale_.z);
        dragLastScale_ = outScale;
        outScale = delta;
        break;
    }
    case GizmoMode::Rotate: {
        if (!dragParamValid_) return true;
        float dx = screenX - dragLastPoint_.x;
        float dy = screenY - dragLastPoint_.y;
        dragLastPoint_ = Vec3(screenX, screenY, 0.0f);
        float arcPx = dx*dragTangent_.x + dy*dragTangent_.y;
        outRotate = quatAxisAngle(dragNormal_, arcPx / dragRadius_);
        break;
    }
    }

    if (config_.mode == GizmoMode::Translate) fireTranslate(outTranslate);
    else if (config_.mode == GizmoMode::Rotate) fireRotate(outRotate);
    else if (config_.mode == GizmoMode::Scale)  fireScale(outScale);
    return true;
}

void GizmoManager::endDrag() {
    if (!isDragging()) return;
    fireEnd(true);
    dragAxis_ = GizmoAxis::None;
}

} // namespace bro::engine
