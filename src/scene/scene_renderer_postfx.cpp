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

#include "tonemap.vert.h"
#include "tonemap.frag.h"
#include "post.vert.h"
#include "blur.frag.h"
#include "tilt_composite.frag.h"
#include "bloom_bright.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// Tonemap pipeline + FBO
// ---------------------------------------------------------------------------

void SceneRenderer::ensureTonemapPipeline() {
    if (tonemapProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kTonemapVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kTonemapFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    tonemapProgram_ = glCreateProgram();
    glAttachShader(tonemapProgram_, vs);
    glAttachShader(tonemapProgram_, fs);
    glLinkProgram(tonemapProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(tonemapProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(tonemapProgram_, sizeof(log), nullptr, log);
        LOG_ERROR("Tonemap program link error: %s", log);
        glDeleteProgram(tonemapProgram_);
        tonemapProgram_ = 0;
        return;
    }

    tmUTex_      = glGetUniformLocation(tonemapProgram_, "uTex");
    tmUExposure_ = glGetUniformLocation(tonemapProgram_, "uExposure");
    tmUGamma_    = glGetUniformLocation(tonemapProgram_, "uGamma");
    tmUMode_     = glGetUniformLocation(tonemapProgram_, "uMode");
    tmUBloomTex_       = glGetUniformLocation(tonemapProgram_, "uBloomTex");
    tmUBloomIntensity_ = glGetUniformLocation(tonemapProgram_, "uBloomIntensity");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &tonemapVAO_);
    glGenBuffers(1, &tonemapVBO_);
    glBindVertexArray(tonemapVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, tonemapVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

void SceneRenderer::ensureTonemapFBO() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    if (tonemapFBO_ && tonemapFBOWidth_ == graph_.canvasWidth_
                   && tonemapFBOHeight_ == graph_.canvasHeight_) return;

    destroyTonemapFBO();

    tonemapFBOWidth_  = graph_.canvasWidth_;
    tonemapFBOHeight_ = graph_.canvasHeight_;

    glGenFramebuffers(1, &tonemapFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);

    glGenTextures(1, &tonemapColorTex_);
    glBindTexture(GL_TEXTURE_2D, tonemapColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tonemapFBOWidth_, tonemapFBOHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tonemapColorTex_, 0);

    // Reuse the mesh FBO's depth-stencil RBO so the post-tonemap unlit overlay
    // pass can depth-test against the scene geometry that was rendered there.
    if (meshDepthRBO_) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, meshDepthRBO_);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Tonemap FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyTonemapFBO() {
    if (tonemapColorTex_) { glDeleteTextures(1, &tonemapColorTex_); tonemapColorTex_ = 0; }
    if (tonemapFBO_)      { glDeleteFramebuffers(1, &tonemapFBO_); tonemapFBO_ = 0; }
    tonemapFBOWidth_ = 0;
    tonemapFBOHeight_ = 0;
}

std::vector<uint8_t> SceneRenderer::readTonemapPixelsRGBA(int& outW, int& outH) {
    // Prefer the tilt-shift output when that pass ran this frame, so direct
    // readback matches what the compositor shows.
    const bool usePost = tiltActive_ && postFBO_;
    const GLuint readFBO = usePost ? postFBO_ : tonemapFBO_;
    outW = usePost ? postWidth_  : tonemapFBOWidth_;
    outH = usePost ? postHeight_ : tonemapFBOHeight_;
    if (!readFBO || outW <= 0 || outH <= 0) {
        outW = outH = 0;
        return {};
    }

    std::vector<uint8_t> px(static_cast<size_t>(outW) * outH * 4);
    GLint prevReadFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
    GLint prevAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFBO));

    // GL origin is bottom-left; CSS / ImageData / putImageData expect top-left.
    const size_t rowBytes = static_cast<size_t>(outW) * 4;
    std::vector<uint8_t> tmp(rowBytes);
    for (int y = 0; y < outH / 2; ++y) {
        uint8_t* a = px.data() + static_cast<size_t>(y) * rowBytes;
        uint8_t* b = px.data() + static_cast<size_t>(outH - 1 - y) * rowBytes;
        std::memcpy(tmp.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, tmp.data(), rowBytes);
    }
    return px;
}

void SceneRenderer::runTonemapPass() {
    ensureTonemapPipeline();
    ensureTonemapFBO();
    if (!tonemapProgram_ || !tonemapFBO_) return;

    // Bright-pass + blur the HDR mesh target before resolving, so the tonemap
    // draw can add the glow in HDR. Leaves bloomActive_/bloomTex_ ready.
    const bool haveBloom = runBloomPrePass();

    glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);
    glViewport(0, 0, tonemapFBOWidth_, tonemapFBOHeight_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(tonemapProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glUniform1i(tmUTex_, 0);
    // Bloom on unit 1 — bind a valid texture even when off (intensity 0).
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, haveBloom ? bloomTex_[0] : meshColorTex_);
    glUniform1i(tmUBloomTex_, 1);
    glUniform1f(tmUBloomIntensity_, haveBloom ? bloomIntensity_ : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(tmUExposure_, exposure_);
    glUniform1f(tmUGamma_, gamma_);
    glUniform1i(tmUMode_, static_cast<int>(toneMap_));

    glBindVertexArray(tonemapVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// HDR bloom pre-pass
// ---------------------------------------------------------------------------

void SceneRenderer::ensureBloomPipeline() {
    ensureTiltShiftPipeline();   // shares the separable-blur program + quad VAO
    if (bloomBrightProgram_) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   kPostVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kBloomBrightFragSrc);
    if (vs && fs) {
        bloomBrightProgram_ = glCreateProgram();
        glAttachShader(bloomBrightProgram_, vs);
        glAttachShader(bloomBrightProgram_, fs);
        glLinkProgram(bloomBrightProgram_);
        GLint ok = 0;
        glGetProgramiv(bloomBrightProgram_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(bloomBrightProgram_, sizeof(log), nullptr, log);
            LOG_ERROR("Bloom bright-pass link error: %s", log);
            glDeleteProgram(bloomBrightProgram_);
            bloomBrightProgram_ = 0;
        } else {
            bbpUTex_       = glGetUniformLocation(bloomBrightProgram_, "uTex");
            bbpUThreshold_ = glGetUniformLocation(bloomBrightProgram_, "uThreshold");
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
}

void SceneRenderer::ensureBloomFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int hw = std::max(1, graph_.canvasWidth_ / 2);
    const int hh = std::max(1, graph_.canvasHeight_ / 2);
    if (bloomFBO_[0] && bloomWidth_ == hw && bloomHeight_ == hh) return;
    destroyBloomFBOs();

    bloomWidth_  = hw;
    bloomHeight_ = hh;
    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &bloomFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[i]);
        glGenTextures(1, &bloomTex_[i]);
        glBindTexture(GL_TEXTURE_2D, bloomTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               bloomTex_[i], 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Bloom FBO %d incomplete: 0x%x", i, status);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyBloomFBOs() {
    for (int i = 0; i < 2; ++i) {
        if (bloomTex_[i]) { glDeleteTextures(1, &bloomTex_[i]); bloomTex_[i] = 0; }
        if (bloomFBO_[i]) { glDeleteFramebuffers(1, &bloomFBO_[i]); bloomFBO_[i] = 0; }
    }
    bloomWidth_ = bloomHeight_ = 0;
}

bool SceneRenderer::runBloomPrePass() {
    bloomActive_ = false;
    if (!bloomEnabled_ || bloomIntensity_ <= 0.0f || !meshColorTex_) return false;

    ensureBloomPipeline();
    ensureBloomFBOs();
    if (!bloomBrightProgram_ || !blurProgram_ || !bloomFBO_[0]) return false;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, bloomWidth_, bloomHeight_);
    glBindVertexArray(tonemapVAO_);
    glActiveTexture(GL_TEXTURE0);

    // Bright-pass: HDR mesh → bloomTex_[0].
    glUseProgram(bloomBrightProgram_);
    glUniform1i(bbpUTex_, 0);
    glUniform1f(bbpUThreshold_, bloomThreshold_);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Separable Gaussian (shared blur program): [0] -H-> [1] -V-> [0].
    const float rx = bloomStrength_ / static_cast<float>(bloomWidth_);
    const float ry = bloomStrength_ / static_cast<float>(bloomHeight_);
    glUseProgram(blurProgram_);
    glUniform1i(blUTex_, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[1]);
    glBindTexture(GL_TEXTURE_2D, bloomTex_[0]);
    glUniform2f(blUDir_, rx, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, bloomTex_[1]);
    glUniform2f(blUDir_, 0.0f, ry);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bloomActive_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Tilt-shift DOF post pass
// ---------------------------------------------------------------------------

void SceneRenderer::ensureTiltShiftPipeline() {
    if (blurProgram_ && tiltProgram_) return;

    if (!blurProgram_) {
        GLuint vs = compileShader(GL_VERTEX_SHADER,   kPostVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kBlurFragSrc);
        if (vs && fs) {
            blurProgram_ = glCreateProgram();
            glAttachShader(blurProgram_, vs);
            glAttachShader(blurProgram_, fs);
            glLinkProgram(blurProgram_);
            GLint ok = 0;
            glGetProgramiv(blurProgram_, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetProgramInfoLog(blurProgram_, sizeof(log), nullptr, log);
                LOG_ERROR("Blur program link error: %s", log);
                glDeleteProgram(blurProgram_);
                blurProgram_ = 0;
            } else {
                blUTex_ = glGetUniformLocation(blurProgram_, "uTex");
                blUDir_ = glGetUniformLocation(blurProgram_, "uDir");
            }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
    }

    if (!tiltProgram_) {
        GLuint vs = compileShader(GL_VERTEX_SHADER,   kPostVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kTiltCompositeFragSrc);
        if (vs && fs) {
            tiltProgram_ = glCreateProgram();
            glAttachShader(tiltProgram_, vs);
            glAttachShader(tiltProgram_, fs);
            glLinkProgram(tiltProgram_);
            GLint ok = 0;
            glGetProgramiv(tiltProgram_, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetProgramInfoLog(tiltProgram_, sizeof(log), nullptr, log);
                LOG_ERROR("Tilt-shift program link error: %s", log);
                glDeleteProgram(tiltProgram_);
                tiltProgram_ = 0;
            } else {
                tsUSharp_       = glGetUniformLocation(tiltProgram_, "uSharp");
                tsUBlur_        = glGetUniformLocation(tiltProgram_, "uBlur");
                tsUFocusCenter_ = glGetUniformLocation(tiltProgram_, "uFocusCenter");
                tsUFocusWidth_  = glGetUniformLocation(tiltProgram_, "uFocusWidth");
                tsUFeather_     = glGetUniformLocation(tiltProgram_, "uFeather");
                tsUSaturation_  = glGetUniformLocation(tiltProgram_, "uSaturation");
                tsUContrast_    = glGetUniformLocation(tiltProgram_, "uContrast");
            }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
    }
}

void SceneRenderer::ensureTiltShiftFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;

    const int hw = std::max(1, graph_.canvasWidth_ / 2);
    const int hh = std::max(1, graph_.canvasHeight_ / 2);

    if (blurFBO_[0] && blurWidth_ == hw && blurHeight_ == hh &&
        postFBO_ && postWidth_ == graph_.canvasWidth_ && postHeight_ == graph_.canvasHeight_) {
        return;
    }
    destroyTiltShiftFBOs();

    // Half-res ping-pong blur targets.
    blurWidth_  = hw;
    blurHeight_ = hh;
    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &blurFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, blurFBO_[i]);
        glGenTextures(1, &blurTex_[i]);
        glBindTexture(GL_TEXTURE_2D, blurTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hw, hh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               blurTex_[i], 0);
    }

    // Full-res composite target.
    postWidth_  = graph_.canvasWidth_;
    postHeight_ = graph_.canvasHeight_;
    glGenFramebuffers(1, &postFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, postFBO_);
    glGenTextures(1, &postColorTex_);
    glBindTexture(GL_TEXTURE_2D, postColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, postWidth_, postHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           postColorTex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Tilt-shift FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyTiltShiftFBOs() {
    for (int i = 0; i < 2; ++i) {
        if (blurTex_[i]) { glDeleteTextures(1, &blurTex_[i]); blurTex_[i] = 0; }
        if (blurFBO_[i]) { glDeleteFramebuffers(1, &blurFBO_[i]); blurFBO_[i] = 0; }
    }
    if (postColorTex_) { glDeleteTextures(1, &postColorTex_); postColorTex_ = 0; }
    if (postFBO_)      { glDeleteFramebuffers(1, &postFBO_); postFBO_ = 0; }
    blurWidth_ = blurHeight_ = 0;
    postWidth_ = postHeight_ = 0;
}

void SceneRenderer::runTiltShiftPass() {
    tiltActive_ = false;
    if (!tiltEnabled_ || !tonemapColorTex_) return;

    ensureTiltShiftPipeline();
    ensureTiltShiftFBOs();
    if (!blurProgram_ || !tiltProgram_ || !postFBO_ || !blurFBO_[0]) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(tonemapVAO_);

    // --- Downsample + separable Gaussian (sharp → blurTex_[1]) -------------
    const float rx = tiltStrength_ / static_cast<float>(blurWidth_);
    const float ry = tiltStrength_ / static_cast<float>(blurHeight_);
    glUseProgram(blurProgram_);
    glUniform1i(blUTex_, 0);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, blurWidth_, blurHeight_);

    // Horizontal: full-res tonemap → blurTex_[0]
    glBindFramebuffer(GL_FRAMEBUFFER, blurFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, tonemapColorTex_);
    glUniform2f(blUDir_, rx, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Vertical: blurTex_[0] → blurTex_[1]
    glBindFramebuffer(GL_FRAMEBUFFER, blurFBO_[1]);
    glBindTexture(GL_TEXTURE_2D, blurTex_[0]);
    glUniform2f(blUDir_, 0.0f, ry);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // --- Composite (sharp + blur → postColorTex_) --------------------------
    glUseProgram(tiltProgram_);
    glBindFramebuffer(GL_FRAMEBUFFER, postFBO_);
    glViewport(0, 0, postWidth_, postHeight_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tonemapColorTex_);
    glUniform1i(tsUSharp_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurTex_[1]);
    glUniform1i(tsUBlur_, 1);
    glUniform1f(tsUFocusCenter_, tiltFocusCenter_);
    glUniform1f(tsUFocusWidth_, tiltFocusWidth_);
    glUniform1f(tsUFeather_, tiltFeather_);
    glUniform1f(tsUSaturation_, tiltSaturation_);
    glUniform1f(tsUContrast_, tiltContrast_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    tiltActive_ = true;
}

}  // namespace bro::scene
