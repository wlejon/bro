#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "billboard.vert.h"
#include "billboard.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

void SceneRenderer::ensureBillboardPipeline() {
    if (bbProgram_) return;

    bbProgram_ = linkProgram(kBillboardVertSrc, kBillboardFragSrc, "Billboard program");

    bbUVP_         = glGetUniformLocation(bbProgram_, "uVP");
    bbUAnchorRel_  = glGetUniformLocation(bbProgram_, "uAnchorRel");
    bbURight_      = glGetUniformLocation(bbProgram_, "uRight");
    bbUUp_         = glGetUniformLocation(bbProgram_, "uUp");
    bbUHalfSize_   = glGetUniformLocation(bbProgram_, "uHalfSize");
    bbUShapeMode_  = glGetUniformLocation(bbProgram_, "uShapeMode");
    bbUColor_      = glGetUniformLocation(bbProgram_, "uColor");
    bbUStroke_     = glGetUniformLocation(bbProgram_, "uStroke");
    bbUStrokeWidth_ = glGetUniformLocation(bbProgram_, "uStrokeWidth");
    bbUTex_        = glGetUniformLocation(bbProgram_, "uTex");
    bbUUvMin_      = glGetUniformLocation(bbProgram_, "uUvMin");
    bbUUvMax_      = glGetUniformLocation(bbProgram_, "uUvMax");

    // Two triangles covering [-1,1] on both axes. Shared across every
    // billboard — only uniforms change per draw.
    static const float quadVerts[12] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &bbVAO_);
    glGenBuffers(1, &bbVBO_);
    glBindVertexArray(bbVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, bbVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Gaussian splat rendering
// ---------------------------------------------------------------------------
// Splats are order-dependent transparency: depth-test against the opaque mesh
// FBO (so geometry occludes them) but don't write depth (splats blend over
// each other in CPU-sorted back-to-front order). Premultiplied "over" matches
// the fragment shader's premultiplied output and the billboard/Skia composite.
void SceneRenderer::renderGaussianSplatNodes() {
    const float eye[3] = {graph_.cameraEye_.x, graph_.cameraEye_.y, graph_.cameraEye_.z};

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    for (auto& [id, node] : graph_.nodes_) {
        if (!node->visible()) continue;
        if (node->type() != SceneNode::Type::GaussianSplat) continue;
        static_cast<GaussianSplatNode*>(node.get())->draw(
            graph_.viewMatrix_.data, graph_.projectionMatrix_.data, eye,
            meshFBOWidth_, meshFBOHeight_);
    }

    // Restore the depth-write default the opaque passes expect.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// ---------------------------------------------------------------------------
// Billboard rendering
// ---------------------------------------------------------------------------

namespace {

// Resolve world-space half-extents for a billboard node.
// ShapeNode uses width/height (or radius for circles), SpriteNode uses
// width/height, HtmlNode converts its pixel layout size by pxPerUnit. All
// defaults keep the quad a reasonable size even if the user forgot to set
// dimensions.
struct BillboardDraw {
    // 0 rect, 1 circle SDF, 2 premul-textured (HtmlNode), 3 ringed disc,
    // 4 straight-alpha textured (SpriteNode).
    int shapeMode = 0;
    float halfW = 0.5f;
    float halfH = 0.5f;
    float color[4] = {1, 1, 1, 1};
    float stroke[4] = {0, 0, 0, 0};
    float strokeWidth = 0.0f;
    GLuint texture = 0;
    float uvMin[2] = {0.0f, 0.0f};
    float uvMax[2] = {1.0f, 1.0f};
};

static inline void color8(float* out, const bromath::Color& c) {
    // Encode linear-float Color back to sRGB float for the billboard shader,
    // which writes sRGB-encoded fragments to a non-linear framebuffer.
    out[0] = bromath::clinearToSrgb(c.r);
    out[1] = bromath::clinearToSrgb(c.g);
    out[2] = bromath::clinearToSrgb(c.b);
    out[3] = c.a;
}

static bool resolveBillboard(SceneNode* node, BillboardDraw& d) {
    using T = SceneNode::Type;
    switch (node->type()) {
    case T::Shape: {
        auto* s = static_cast<ShapeNode*>(node);
        const Vec3& scl = node->scale();
        switch (s->shape()) {
        case ShapeNode::Shape::Rect:
        case ShapeNode::Shape::RoundRect:
            d.shapeMode = 0;
            d.halfW = 0.5f * s->width()  * scl.x;
            d.halfH = 0.5f * s->height() * scl.y;
            break;
        case ShapeNode::Shape::Circle:
            d.shapeMode = 1;
            d.halfW = s->radius() * scl.x;
            d.halfH = s->radius() * scl.y;
            break;
        case ShapeNode::Shape::Ellipse:
            d.shapeMode = 1;
            d.halfW = s->radiusX() * scl.x;
            d.halfH = s->radiusY() * scl.y;
            break;
        default:
            // Polygon / line are 2D-only for world-anchored billboards; fall
            // back to a solid rect bounded by width/height.
            d.shapeMode = 0;
            d.halfW = 0.5f * s->width()  * scl.x;
            d.halfH = 0.5f * s->height() * scl.y;
            break;
        }
        color8(d.color,  s->fillColor());
        if (!s->hasFill()) d.color[3] = 0.0f;
        color8(d.stroke, s->strokeColor());
        // Map stroke width from world units to UV space (0..1 per half-size).
        float uvRef = std::max(d.halfW, d.halfH) * 2.0f;
        d.strokeWidth = (s->hasStroke() && uvRef > 0.0f)
                      ? (s->strokeWidth() / uvRef)
                      : 0.0f;
        return true;
    }
    case T::Sprite: {
        auto* s = static_cast<SpriteNode*>(node);
        const Vec3& scl = node->scale();
        // Default the world-quad size to the sheet frame (or full image)
        // when the user didn't set explicit width/height — saves callers
        // from having to compute world-unit extents twice.
        float worldW = s->width();
        float worldH = s->height();
        if (worldW <= 0.0f || worldH <= 0.0f) {
            float sx, sy, sw, sh;
            if (s->currentSheetRect(sx, sy, sw, sh)) {
                if (worldW <= 0.0f) worldW = sw;
                if (worldH <= 0.0f) worldH = sh;
            } else if (s->imageWidth() > 0 && s->imageHeight() > 0) {
                if (worldW <= 0.0f) worldW = static_cast<float>(s->imageWidth());
                if (worldH <= 0.0f) worldH = static_cast<float>(s->imageHeight());
            }
        }
        d.shapeMode = 4;  // straight-alpha textured
        d.halfW = 0.5f * worldW * scl.x;
        d.halfH = 0.5f * worldH * scl.y;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.color[3] = s->opacity();
        d.texture = s->textureId();
        s->currentUvRect(d.uvMin[0], d.uvMin[1], d.uvMax[0], d.uvMax[1]);
        if (d.texture == 0) d.color[3] = 0.0f;
        return true;
    }
    case T::Html: {
        auto* h = static_cast<HtmlNode*>(node);
        const Vec3& scl = node->scale();
        float ppu = h->pxPerUnit();
        if (ppu <= 0.0f) ppu = 100.0f;
        d.shapeMode = 2;
        d.halfW = 0.5f * (h->layoutWidth()  / ppu) * scl.x;
        d.halfH = 0.5f * (h->layoutHeight() / ppu) * scl.y;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.color[3] = 1.0f;
        d.texture = h->textureId();
        if (d.texture == 0) d.color[3] = 0.0f;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

void SceneRenderer::renderBillboardNode(SceneNode* node) {
    BillboardDraw d;
    if (!resolveBillboard(node, d)) return;
    if (d.color[3] <= 0.0f && d.shapeMode != 2) return; // invisible shape

    // Anchor in camera-relative space (same precision trick as mesh path).
    const Vec3 anchor = node->worldAnchor();
    const float ax = anchor.x - graph_.cameraEye_.x;
    const float ay = anchor.y - graph_.cameraEye_.y;
    const float az = anchor.z - graph_.cameraEye_.z;

    // Camera basis in world space from rows of view matrix (see renderMesh).
    const Vec3 camRight   {graph_.viewMatrix_.at(0, 0), graph_.viewMatrix_.at(0, 1), graph_.viewMatrix_.at(0, 2)};
    const Vec3 camUp      {graph_.viewMatrix_.at(1, 0), graph_.viewMatrix_.at(1, 1), graph_.viewMatrix_.at(1, 2)};
    const Vec3 camForward { -graph_.viewMatrix_.at(2, 0), -graph_.viewMatrix_.at(2, 1), -graph_.viewMatrix_.at(2, 2)};

    Vec3 right = camRight;
    Vec3 up    = camUp;

    if (node->billboardMode() == SceneNode::BillboardMode::YLock) {
        // Y-lock degenerates when the camera looks nearly straight up/down —
        // the horizontal right vector collapses. Fall back to full billboard.
        if (std::fabs(camForward.y) < 0.99f) {
            up = {0.0f, 1.0f, 0.0f};
            Vec3 flatRight{camRight.x, 0.0f, camRight.z};
            float len = bromath::vlen(flatRight);
            if (len > 1e-5f) {
                right = flatRight * (1.0f / len);
            }
        }
    }

    glUniform3f(bbUAnchorRel_, ax, ay, az);
    glUniform3f(bbURight_, right.x, right.y, right.z);
    glUniform3f(bbUUp_,    up.x,    up.y,    up.z);
    glUniform2f(bbUHalfSize_, d.halfW, d.halfH);
    glUniform1i(bbUShapeMode_, d.shapeMode);
    glUniform4fv(bbUColor_,  1, d.color);
    glUniform4fv(bbUStroke_, 1, d.stroke);
    glUniform1f(bbUStrokeWidth_, d.strokeWidth);
    glUniform2f(bbUUvMin_, d.uvMin[0], d.uvMin[1]);
    glUniform2f(bbUUvMax_, d.uvMax[0], d.uvMax[1]);

    if (d.shapeMode == 2 || d.shapeMode == 4) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d.texture);
        glUniform1i(bbUTex_, 0);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Kind-specific visual tuning so all three light types are visually
// distinguishable when overlapping in screen space.
//   Directional: larger disc with a thick white ring (sun-like).
//   Point:       medium disc with a faint outer ring.
//   Spot:        small disc with a heavy colored ring (cone-ish).
void SceneRenderer::renderLightIcon(LightNode* light) {
    if (!light) return;

    const Mat4& M = light->worldMatrix();
    const float ax = M.at(0, 3) - graph_.cameraEye_.x;
    const float ay = M.at(1, 3) - graph_.cameraEye_.y;
    const float az = M.at(2, 3) - graph_.cameraEye_.z;

    // Full-billboard (camera-facing) — icons always face the camera.
    const Vec3 camRight{graph_.viewMatrix_.at(0, 0), graph_.viewMatrix_.at(0, 1), graph_.viewMatrix_.at(0, 2)};
    const Vec3 camUp   {graph_.viewMatrix_.at(1, 0), graph_.viewMatrix_.at(1, 1), graph_.viewMatrix_.at(1, 2)};

    const Vec3& lc = light->color();
    // Keep icon visible even for lights with very dark configured colors.
    const float lum = 0.299f * lc.x + 0.587f * lc.y + 0.114f * lc.z;
    const float lift = lum < 0.2f ? 0.2f : 0.0f;
    float core[4] = { lc.x + lift, lc.y + lift, lc.z + lift, 1.0f };

    float ring[4];
    float half, strokeT;

    switch (light->kind()) {
    case LightNode::Kind::Directional:
        half = 0.30f;
        strokeT = 0.18f;
        ring[0] = ring[1] = ring[2] = 1.0f; ring[3] = 1.0f;
        break;
    case LightNode::Kind::Point:
        half = 0.22f;
        strokeT = 0.12f;
        ring[0] = core[0] * 0.5f;
        ring[1] = core[1] * 0.5f;
        ring[2] = core[2] * 0.5f;
        ring[3] = 1.0f;
        break;
    case LightNode::Kind::Spot:
    default:
        half = 0.22f;
        strokeT = 0.28f;
        ring[0] = std::min(core[0] * 0.8f, 1.0f);
        ring[1] = std::min(core[1] * 0.8f, 1.0f);
        ring[2] = std::min(core[2] * 0.8f, 1.0f);
        ring[3] = 1.0f;
        break;
    }

    glUniform3f(bbUAnchorRel_, ax, ay, az);
    glUniform3f(bbURight_, camRight.x, camRight.y, camRight.z);
    glUniform3f(bbUUp_,    camUp.x,    camUp.y,    camUp.z);
    glUniform2f(bbUHalfSize_, half, half);
    glUniform1i(bbUShapeMode_, 3);  // ringed disc
    glUniform4fv(bbUColor_,  1, core);
    glUniform4fv(bbUStroke_, 1, ring);
    glUniform1f(bbUStrokeWidth_, strokeT);
    glUniform2f(bbUUvMin_, 0.0f, 0.0f);
    glUniform2f(bbUUvMax_, 1.0f, 1.0f);

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

}  // namespace bro::scene
