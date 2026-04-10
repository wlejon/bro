#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"
#include "physics/physics_world.h"
#include "util/log.h"

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

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* kMeshFragSrc = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;

uniform vec4 uColor;
uniform vec3 uLightDir;
uniform vec3 uCameraPos;
uniform float uEmissive;
uniform int uUseVertexColor;

out vec4 FragColor;

void main() {
    vec3 baseColor = (uUseVertexColor == 1) ? vColor.rgb : uColor.rgb;
    float baseAlpha = (uUseVertexColor == 1) ? vColor.a : uColor.a;

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
    FragColor = vec4(mix(lit, baseColor, uEmissive), baseAlpha);
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

    // Check if we have any mesh nodes to render
    bool hasMeshNodes = false;
    for (auto& [id, node] : nodes_) {
        if (node->type() == SceneNode::Type::Mesh && node->visible()) {
            hasMeshNodes = true;
            break;
        }
    }

    // Set up 3D pipeline if needed
    if (hasMeshNodes && canvasWidth_ > 0 && canvasHeight_ > 0) {
        ensureMeshPipeline();
        ensureMeshFBO();

        if (meshProgram_ && meshFBO_) {
            // Bind FBO and set up GL state for 3D rendering
            glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);
            glViewport(0, 0, meshFBOWidth_, meshFBOHeight_);
            // Reset GL state that Skia/Ganesh may have changed
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent background
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);

            glUseProgram(meshProgram_);

            // Set per-frame uniforms
            Vec3 lightDir = Vec3(0.3f, 1.0f, 0.5f).normalized();
            glUniform3f(uLightDir_, lightDir.x, lightDir.y, lightDir.z);
            glUniform3f(uCameraPos_, cameraEye_.x, cameraEye_.y, cameraEye_.z);

            hasMeshContent_ = true;
        }
    }

    // Depth-first render traversal (handles both 2D and 3D nodes)
    renderNode(root_.get());

    // Tear down 3D state
    if (hasMeshContent_) {
        glUseProgram(0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

void SceneGraph::renderNode(SceneNode* node) {
    if (!node->visible()) return;

    if (node->type() == SceneNode::Type::Mesh && hasMeshContent_) {
        renderMeshNode(static_cast<MeshNode*>(node));
    } else {
        node->onRender(*this);
    }

    for (auto* child : node->children()) {
        renderNode(child);
    }
}

void SceneGraph::renderMeshNode(MeshNode* mesh) {
    // Compute MVP = projection * view * model(world)
    Mat4 vp = projectionMatrix_ * viewMatrix_;
    Mat4 mvp = vp * mesh->worldMatrix();

    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, mesh->worldMatrix().data());
    glUniform4fv(uColor_, 1, mesh->color());
    glUniform1f(uEmissive_, mesh->emissive());
    glUniform1i(uUseVertexColor_, mesh->hasVertexColors() ? 1 : 0);

    // Per-mesh polygon offset (depth bias). Used by callers that need to
    // layer co-located meshes — e.g. terrain LOD shells that overlap and need
    // the high-detail mesh to consistently win the depth test.
    float pf = mesh->depthBiasFactor();
    float pu = mesh->depthBiasUnits();
    if (pf != 0.0f || pu != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pf, pu);
    }

    mesh->onRender(*this);

    if (pf != 0.0f || pu != 0.0f) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
}

} // namespace bro::scene
