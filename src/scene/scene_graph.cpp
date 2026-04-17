#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"
#include "physics/physics_world.h"
#include "render/skia_backend.h"
#include "layout/font_manager.h"
#include "dom/document.h"
#include "util/log.h"
#include "brogameagent/world.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Mesh shader sources (GLSL 330 core)
// ---------------------------------------------------------------------------

static const char* kMeshVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform int uUseVertexColor;
uniform vec3 uCameraPos;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(worldPos.xyz - uCameraPos);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* kMeshFragSrc = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vCamDist;

uniform vec4 uColor;
uniform vec3 uLightDir;
uniform vec3 uCameraPos;
uniform float uEmissive;
uniform int uUseVertexColor;
uniform int uUseTexture;
uniform sampler2D uBaseColorTex;
uniform float uFogStart;
uniform float uFogEnd;
uniform vec3 uFogColor;
uniform float uNearClip;

out vec4 FragColor;

void main() {
    // Discard fragments within finer LOD coverage
    if (uNearClip > 0.0 && vCamDist < uNearClip) discard;

    vec3 baseColor;
    float baseAlpha;
    if (uUseTexture == 1) {
        vec4 tex = texture(uBaseColorTex, vUV);
        baseColor = tex.rgb * uColor.rgb;
        baseAlpha = tex.a   * uColor.a;
    } else if (uUseVertexColor == 1) {
        baseColor = vColor.rgb;
        baseAlpha = vColor.a;
    } else {
        baseColor = uColor.rgb;
        baseAlpha = uColor.a;
    }

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);

    // Ambient + diffuse + specular
    float ambient = 0.15;
    float diff = max(dot(N, L), 0.0);

    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.3;

    float light = ambient + diff * 0.7 + spec;
    vec3 lit = baseColor * light;
    vec3 color = mix(lit, baseColor, uEmissive);

    // Distance fog — fade to sky color at far boundary
    if (uFogEnd > 0.0) {
        float fogFactor = clamp((vCamDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0);
        fogFactor = fogFactor * fogFactor; // smooth quadratic ramp
        color = mix(color, uFogColor, fogFactor);
        baseAlpha = mix(baseAlpha, 0.0, fogFactor);
    }

    FragColor = vec4(color, baseAlpha);
}
)";

// ---------------------------------------------------------------------------
// Billboard shader sources — a single camera-facing textured/filled quad.
// The vertex shader places a unit quad at a world anchor using camera basis
// vectors supplied by the CPU. Positions are computed in a *camera-relative*
// frame (anchor - cameraEye), which matches renderMeshNode and avoids float
// precision loss at large world coordinates.
// ---------------------------------------------------------------------------

static const char* kBillboardVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aQuad;  // [-1..1] corner

uniform mat4 uVP;            // projection * viewRot (no translation)
uniform vec3 uAnchorRel;     // worldAnchor - cameraEye
uniform vec3 uRight;         // billboard right axis, world space
uniform vec3 uUp;            // billboard up axis, world space
uniform vec2 uHalfSize;      // world-space half-extents

out vec2 vUV;

void main() {
    vec3 worldRel = uAnchorRel
                  + uRight * (aQuad.x * uHalfSize.x)
                  + uUp    * (aQuad.y * uHalfSize.y);
    // Flip Y so UV origin is top-left (matches Skia/CSS pixel layout).
    vUV = vec2(aQuad.x * 0.5 + 0.5, 0.5 - aQuad.y * 0.5);
    gl_Position = uVP * vec4(worldRel, 1.0);
}
)";

static const char* kBillboardFragSrc = R"(
#version 330 core
in vec2 vUV;

uniform int uShapeMode;      // 0 = rect, 1 = circle SDF, 2 = textured
uniform vec4 uColor;         // rect / circle fill, or tint for texture
uniform vec4 uStroke;
uniform float uStrokeWidth;  // in UV units (0 = no stroke)
uniform sampler2D uTex;

out vec4 FragColor;

void main() {
    if (uShapeMode == 0) {
        // Rect: solid fill with optional inset stroke.
        vec4 c = uColor;
        if (uStrokeWidth > 0.0) {
            vec2 d = min(vUV, 1.0 - vUV);
            float border = min(d.x, d.y);
            if (border < uStrokeWidth) c = uStroke;
        }
        if (c.a <= 0.0) discard;
        // Straight-alpha input — premultiply for "over" blend.
        FragColor = vec4(c.rgb * c.a, c.a);
    } else if (uShapeMode == 1) {
        // Circle SDF centered on UV (0.5, 0.5), radius 0.5.
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float a = uColor.a * alpha;
        FragColor = vec4(uColor.rgb * a, a);
    } else {
        // Textured (premultiplied alpha from Skia surfaces).
        vec4 tex = texture(uTex, vUV);
        vec4 c = tex * uColor; // uColor.a tints premultiplied texture
        if (c.a <= 0.0) discard;
        FragColor = c;
    }
}
)";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SceneGraph::SceneGraph() {
    root_ = std::make_unique<SceneNode>("__root__");
}

SceneGraph::~SceneGraph() {
    root_->traverse([](SceneNode* n) {
        for (auto* c : n->children()) {
        }
    });
    nodes_.clear();

    // Destroy GL resources
    destroyMeshFBO();
    if (meshProgram_) { glDeleteProgram(meshProgram_); meshProgram_ = 0; }
    if (bbProgram_) { glDeleteProgram(bbProgram_); bbProgram_ = 0; }
    if (bbVBO_) { glDeleteBuffers(1, &bbVBO_); bbVBO_ = 0; }
    if (bbVAO_) { glDeleteVertexArrays(1, &bbVAO_); bbVAO_ = 0; }
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

HtmlNode* SceneGraph::createHtml(const std::string& name) {
    auto node = std::make_unique<HtmlNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

void SceneGraph::setCanvasSize(int w, int h) {
    canvasWidth_ = w;
    canvasHeight_ = h;
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

SceneNode* SceneGraph::findByName(const std::string& name) const {
    for (auto& [id, node] : nodes_) {
        if (node->name() == name) return node.get();
    }
    return nullptr;
}

void SceneGraph::setCamera(float fovY, float aspect, float nearZ, float farZ,
                           const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::perspective(fovY, aspect, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
}

void SceneGraph::setCameraQuat(float fovY, float aspect, float nearZ, float farZ,
                               const Vec3& eye, const Quat& orientation) {
    projectionMatrix_ = Mat4::perspective(fovY, aspect, nearZ, farZ);
    // View matrix = inverse camera transform. For unit quaternion, inverse = conjugate.
    Quat inv = orientation.conjugate();
    viewMatrix_ = Mat4::fromQuat(inv);
    // Translate in rotated space: view translation = inv.rotate(-eye)
    Vec3 negEye = inv.rotate({-eye.x, -eye.y, -eye.z});
    viewMatrix_.m[3][0] = negEye.x;
    viewMatrix_.m[3][1] = negEye.y;
    viewMatrix_.m[3][2] = negEye.z;
    cameraEye_ = eye;
}

void SceneGraph::setCameraOrtho(float left, float right, float bottom, float top,
                                float nearZ, float farZ,
                                const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::orthographic(left, right, bottom, top, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
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

void SceneGraph::attachAIWorld(brogameagent::World* world, float stepHz, int maxStepsPerFrame) {
    if (!world) { aiTicker_.reset(); return; }
    aiTicker_ = std::make_unique<AIWorldTicker>(world, stepHz, maxStepsPerFrame);
}

void SceneGraph::detachAIWorld() {
    aiTicker_.reset();
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

// ---------------------------------------------------------------------------
// Mesh GL pipeline (lazy init)
// ---------------------------------------------------------------------------

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("Mesh shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void SceneGraph::ensureMeshPipeline() {
    if (meshProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kMeshVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kMeshFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    meshProgram_ = glCreateProgram();
    glAttachShader(meshProgram_, vs);
    glAttachShader(meshProgram_, fs);
    glLinkProgram(meshProgram_);

    GLint ok = 0;
    glGetProgramiv(meshProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(meshProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Mesh program link error: %s", log);
        glDeleteProgram(meshProgram_);
        meshProgram_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (meshProgram_) {
        uMVP_ = glGetUniformLocation(meshProgram_, "uMVP");
        uModel_ = glGetUniformLocation(meshProgram_, "uModel");
        uColor_ = glGetUniformLocation(meshProgram_, "uColor");
        uLightDir_ = glGetUniformLocation(meshProgram_, "uLightDir");
        uCameraPos_ = glGetUniformLocation(meshProgram_, "uCameraPos");
        uEmissive_ = glGetUniformLocation(meshProgram_, "uEmissive");
        uUseVertexColor_ = glGetUniformLocation(meshProgram_, "uUseVertexColor");
        uUseTexture_     = glGetUniformLocation(meshProgram_, "uUseTexture");
        uBaseColorTex_   = glGetUniformLocation(meshProgram_, "uBaseColorTex");
        uFogStart_ = glGetUniformLocation(meshProgram_, "uFogStart");
        uFogEnd_ = glGetUniformLocation(meshProgram_, "uFogEnd");
        uFogColor_ = glGetUniformLocation(meshProgram_, "uFogColor");
        uNearClip_ = glGetUniformLocation(meshProgram_, "uNearClip");
    }
}

void SceneGraph::ensureMeshFBO() {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return;
    if (meshFBO_ && meshFBOWidth_ == canvasWidth_ && meshFBOHeight_ == canvasHeight_) return;

    destroyMeshFBO();

    meshFBOWidth_ = canvasWidth_;
    meshFBOHeight_ = canvasHeight_;

    glGenFramebuffers(1, &meshFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);

    // Color attachment (RGBA8)
    glGenTextures(1, &meshColorTex_);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, meshFBOWidth_, meshFBOHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, meshColorTex_, 0);

    // Depth renderbuffer
    glGenRenderbuffers(1, &meshDepthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, meshDepthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, meshFBOWidth_, meshFBOHeight_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, meshDepthRBO_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Mesh FBO incomplete: 0x%x", status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneGraph::destroyMeshFBO() {
    if (meshDepthRBO_) { glDeleteRenderbuffers(1, &meshDepthRBO_); meshDepthRBO_ = 0; }
    if (meshColorTex_) { glDeleteTextures(1, &meshColorTex_); meshColorTex_ = 0; }
    if (meshFBO_) { glDeleteFramebuffers(1, &meshFBO_); meshFBO_ = 0; }
    meshFBOWidth_ = 0;
    meshFBOHeight_ = 0;
}

void SceneGraph::ensureBillboardPipeline() {
    if (bbProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kBillboardVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kBillboardFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    bbProgram_ = glCreateProgram();
    glAttachShader(bbProgram_, vs);
    glAttachShader(bbProgram_, fs);
    glLinkProgram(bbProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(bbProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(bbProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Billboard program link error: %s", log);
        glDeleteProgram(bbProgram_);
        bbProgram_ = 0;
        return;
    }

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
// Render
// ---------------------------------------------------------------------------

void SceneGraph::render() {
    hasMeshContent_ = false;

    // 2D render path (CanvasScene)
    if (canvasScene_) {
        canvasScene_->reset();
        canvasScene_->save();
        canvasScene_->translate(-cameraX_, -cameraY_);
        if (cameraZoom_ != 1.0f) {
            canvasScene_->scale(cameraZoom_, cameraZoom_);
        }
    }

    // Check for 3D content: mesh nodes OR world-anchored Shape/Sprite/Html.
    // Both render into the mesh FBO (depth-tested against each other) so
    // they share the same setup path.
    bool hasMeshNodes = false;
    bool hasBillboardNodes = false;
    for (auto& [id, node] : nodes_) {
        if (!node->visible()) continue;
        if (node->type() == SceneNode::Type::Mesh) hasMeshNodes = true;
        else if (node->hasWorldAnchor())           hasBillboardNodes = true;
        if (hasMeshNodes && hasBillboardNodes) break;
    }

    // Resolve the gizmo overlay up-front so it can force the 3D pass even
    // when the canvas has no other 3D content. Cached and replayed below.
    std::vector<MeshNode*> gizmoMeshes;
    if (gizmoProvider_) gizmoMeshes = gizmoProvider_(this);
    const bool hasGizmo = !gizmoMeshes.empty();

    const bool has3D = (hasMeshNodes || hasBillboardNodes || hasGizmo)
                       && canvasWidth_ > 0 && canvasHeight_ > 0;

    if (has3D) {
        ensureMeshPipeline();
        if (hasBillboardNodes) ensureBillboardPipeline();
        ensureMeshFBO();

        if (meshFBO_) {
            glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);
            glViewport(0, 0, meshFBOWidth_, meshFBOHeight_);
            // Reset state that Ganesh may have changed.
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);

            hasMeshContent_ = true;

            // --- Mesh pass --------------------------------------------------
            if (hasMeshNodes && meshProgram_) {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glUseProgram(meshProgram_);

                Vec3 lightDir = Vec3(0.3f, 1.0f, 0.5f).normalized();
                glUniform3f(uLightDir_, lightDir.x, lightDir.y, lightDir.z);
                glUniform3f(uCameraPos_, 0.0f, 0.0f, 0.0f);
                glUniform1f(uFogStart_, fogStart_);
                glUniform1f(uFogEnd_, fogEnd_);
                glUniform3f(uFogColor_, fogColor_[0], fogColor_[1], fogColor_[2]);

                std::function<void(SceneNode*)> walkMesh = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->type() == SceneNode::Type::Mesh) {
                        renderMeshNode(static_cast<MeshNode*>(n));
                    }
                    for (auto* c : n->children()) walkMesh(c);
                };
                walkMesh(root_.get());

                glDisable(GL_CULL_FACE);
            }

            // --- Billboard pass --------------------------------------------
            // Depth test on (occluded behind geometry), depth write off (so
            // multiple billboards don't occlude each other).
            if (hasBillboardNodes && bbProgram_) {
                glUseProgram(bbProgram_);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                // Premultiplied "over" — matches both our SDF/rect fills (we
                // premultiply in the fragment shader) and Skia textures (which
                // produce premultiplied output).
                glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                // View-rotation-only matrix: in camera-relative space the eye
                // is at origin, so only orientation remains.
                Mat4 viewRot = viewMatrix_;
                viewRot.m[3][0] = 0.0f;
                viewRot.m[3][1] = 0.0f;
                viewRot.m[3][2] = 0.0f;
                Mat4 vp = projectionMatrix_ * viewRot;
                glUniformMatrix4fv(bbUVP_, 1, GL_FALSE, vp.data());
                glBindVertexArray(bbVAO_);

                std::function<void(SceneNode*)> walkBB = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->hasWorldAnchor()) {
                        renderBillboardNode(n);
                    }
                    for (auto* c : n->children()) walkBB(c);
                };
                walkBB(root_.get());

                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }

            // --- Gizmo pass ------------------------------------------------
            // Engine-owned overlay handles (translate arrows today; rotate
            // rings + scale boxes in later phases). Drawn with depth-test
            // disabled so handles are always grabbable, even when co-located
            // with scene geometry. Mesh program + shared uniforms mirror the
            // mesh pass above.
            if (hasGizmo && meshProgram_) {
                {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glDisable(GL_DEPTH_TEST);
                    glUseProgram(meshProgram_);

                    Vec3 lightDir = Vec3(0.3f, 1.0f, 0.5f).normalized();
                    glUniform3f(uLightDir_, lightDir.x, lightDir.y, lightDir.z);
                    glUniform3f(uCameraPos_, 0.0f, 0.0f, 0.0f);
                    glUniform1f(uFogStart_, 0.0f);
                    glUniform1f(uFogEnd_, 0.0f);
                    glUniform3f(uFogColor_, 0.0f, 0.0f, 0.0f);

                    for (MeshNode* mn : gizmoMeshes) {
                        if (!mn) continue;
                        renderMeshNode(mn);
                    }

                    glDisable(GL_CULL_FACE);
                    glEnable(GL_DEPTH_TEST);
                    hasMeshContent_ = true;
                }
            }

            glUseProgram(0);
            glDisable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    // --- 2D canvas pass ---------------------------------------------------
    // Render non-world-anchored Shape/Sprite into the 2D overlay layer.
    // World-anchored nodes already rendered into the FBO above.
    {
        std::function<void(SceneNode*)> walk2D = [&](SceneNode* n) {
            if (!n->visible()) return;
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

    // Notify the DOM element of the current FBO texture for compositing
    if (fboTexCb_) {
        fboTexCb_(hasMeshContent_ ? meshColorTex_ : 0);
    }
}

bool SceneGraph::unprojectLocal(float localX, float localY,
                                Vec3& outOrigin, Vec3& outDir) const {
    if (canvasWidth_ <= 0 || canvasHeight_ <= 0) return false;
    const auto& P = projectionMatrix_;
    const auto& V = viewMatrix_;
    float m11 = P.m[1][1];
    if (!std::isfinite(m11) || m11 <= 0.0f) return false;
    float tanHalfFov = 1.0f / m11;
    // View rows (camera basis in world space).
    // Row 0 = right, row 1 = up, row 2 = -forward (camera looks along -Z).
    Vec3 right  (V.m[0][0], V.m[1][0], V.m[2][0]);
    Vec3 up     (V.m[0][1], V.m[1][1], V.m[2][1]);
    Vec3 forward(-V.m[0][2], -V.m[1][2], -V.m[2][2]);

    float aspect = static_cast<float>(canvasWidth_) / static_cast<float>(canvasHeight_);
    float nx = (2.0f * localX / static_cast<float>(canvasWidth_)) - 1.0f;
    float ny = 1.0f - (2.0f * localY / static_cast<float>(canvasHeight_));

    Vec3 dir = forward
             + right * (nx * aspect * tanHalfFov)
             + up    * (ny * tanHalfFov);
    outDir    = dir.normalized();
    outOrigin = cameraEye_;
    return true;
}

void SceneGraph::renderNode(SceneNode* /*node*/) {
    // Retained for ABI stability but unused now that render() performs
    // explicit mesh / billboard / 2D passes.
}

void SceneGraph::renderMeshNode(MeshNode* mesh) {
    // Camera-relative rendering: offset model position by camera to avoid
    // float precision issues at large world coordinates (planet scale).
    Mat4 model = mesh->worldMatrix();
    model.m[3][0] -= cameraEye_.x;
    model.m[3][1] -= cameraEye_.y;
    model.m[3][2] -= cameraEye_.z;

    // View matrix without translation (rotation only) since model is now
    // camera-relative
    Mat4 viewRot = viewMatrix_;
    viewRot.m[3][0] = 0.0f;
    viewRot.m[3][1] = 0.0f;
    viewRot.m[3][2] = 0.0f;

    Mat4 mvp = projectionMatrix_ * viewRot * model;

    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.data());
    glUniform4fv(uColor_, 1, mesh->color());
    glUniform1f(uEmissive_, mesh->emissive());
    glUniform1i(uUseVertexColor_, mesh->hasVertexColors() ? 1 : 0);
    glUniform1f(uNearClip_, mesh->nearClipDist());

    // Bind baseColor texture if present. Texture wins over vertex-color when
    // both are set — matches glTF PBR "baseColorTexture * baseColorFactor".
    bool bindTex = mesh->hasBaseColorTexture();
    if (bindTex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mesh->baseColorTextureId());
        glUniform1i(uBaseColorTex_, 0);
        glUniform1i(uUseTexture_, 1);
    } else {
        glUniform1i(uUseTexture_, 0);
    }

    // Per-mesh polygon offset (depth bias). Used by callers that need to
    // layer co-located meshes — e.g. terrain LOD shells that overlap and need
    // the high-detail mesh to consistently win the depth test.
    float pf = mesh->depthBiasFactor();
    float pu = mesh->depthBiasUnits();
    if (pf != 0.0f || pu != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pf, pu);
    }

    // Alpha blending for translucent meshes (uniform color alpha < 1).
    // Depth writes are disabled so multiple translucent surfaces don't
    // occlude each other; opaque meshes still occlude translucent ones via
    // the unchanged depth test. Separate alpha function ensures the final
    // FBO stays composable over the 2D backdrop (standard "over" operator).
    bool translucent = mesh->color()[3] < 1.0f;
    if (translucent) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    }

    mesh->onRender(*this);

    if (translucent) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    if (pf != 0.0f || pu != 0.0f) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
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
    int shapeMode = 0;        // 0 rect, 1 circle, 2 textured
    float halfW = 0.5f;
    float halfH = 0.5f;
    float color[4] = {1, 1, 1, 1};
    float stroke[4] = {0, 0, 0, 0};
    float strokeWidth = 0.0f;
    GLuint texture = 0;
};

static inline void color8(float* out, const Color& c) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
    out[3] = c.a / 255.0f;
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
        d.shapeMode = 2;
        d.halfW = 0.5f * s->width()  * scl.x;
        d.halfH = 0.5f * s->height() * scl.y;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.color[3] = s->opacity();
        // Texture upload for SpriteNode billboards is a deliberate follow-up;
        // without a texture the shader has nothing to sample, so fade out.
        d.texture = 0;
        d.color[3] = 0.0f;
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

void SceneGraph::renderBillboardNode(SceneNode* node) {
    BillboardDraw d;
    if (!resolveBillboard(node, d)) return;
    if (d.color[3] <= 0.0f && d.shapeMode != 2) return; // invisible shape

    // Anchor in camera-relative space (same precision trick as mesh path).
    const Vec3 anchor = node->worldAnchor();
    const float ax = anchor.x - cameraEye_.x;
    const float ay = anchor.y - cameraEye_.y;
    const float az = anchor.z - cameraEye_.z;

    // Camera basis in world space from rows of view matrix (see renderMesh).
    const Vec3 camRight   {viewMatrix_.m[0][0], viewMatrix_.m[1][0], viewMatrix_.m[2][0]};
    const Vec3 camUp      {viewMatrix_.m[0][1], viewMatrix_.m[1][1], viewMatrix_.m[2][1]};
    const Vec3 camForward { -viewMatrix_.m[0][2], -viewMatrix_.m[1][2], -viewMatrix_.m[2][2]};

    Vec3 right = camRight;
    Vec3 up    = camUp;

    if (node->billboardMode() == SceneNode::BillboardMode::YLock) {
        // Y-lock degenerates when the camera looks nearly straight up/down —
        // the horizontal right vector collapses. Fall back to full billboard.
        if (std::fabs(camForward.y) < 0.99f) {
            up = {0.0f, 1.0f, 0.0f};
            Vec3 flatRight{camRight.x, 0.0f, camRight.z};
            float len = flatRight.length();
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

    if (d.shapeMode == 2) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d.texture);
        glUniform1i(bbUTex_, 0);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// ---------------------------------------------------------------------------
// HtmlNode rasterization — runs on the main/GL thread before scene render
// so layout of the detached Documents stays serialized with JS mutations.
// ---------------------------------------------------------------------------

void SceneGraph::materializeHtmlNodes(render::SkiaRenderer* renderer,
                                      layout::FontManager* fontManager) {
    if (!renderer || !fontManager) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() != SceneNode::Type::Html) continue;
        auto* hn = static_cast<HtmlNode*>(node.get());
        hn->materializePending(renderer, fontManager);
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
