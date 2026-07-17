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
#include "ssao.frag.h"
#include "dof_composite.frag.h"
#include "fxaa.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// Tonemap pipeline + FBO
// ---------------------------------------------------------------------------

void SceneRenderer::ensureTonemapPipeline() {
    if (tonemapProgram_) return;

    tonemapProgram_ = linkProgram(kTonemapVertSrc, kTonemapFragSrc, "Tonemap program");

    tmUTex_      = glGetUniformLocation(tonemapProgram_, "uTex");
    tmUExposure_ = glGetUniformLocation(tonemapProgram_, "uExposure");
    tmUGamma_    = glGetUniformLocation(tonemapProgram_, "uGamma");
    tmUMode_     = glGetUniformLocation(tonemapProgram_, "uMode");
    tmUBloomTex_       = glGetUniformLocation(tonemapProgram_, "uBloomTex");
    tmUBloomIntensity_ = glGetUniformLocation(tonemapProgram_, "uBloomIntensity");
    tmUSSAOTex_        = glGetUniformLocation(tonemapProgram_, "uSSAOTex");
    tmUSSAOIntensity_  = glGetUniformLocation(tonemapProgram_, "uSSAOIntensity");
    tmULUTTex_         = glGetUniformLocation(tonemapProgram_, "uLUTTex");
    tmULUTAmount_      = glGetUniformLocation(tonemapProgram_, "uLUTAmount");
    tmULUTScale_       = glGetUniformLocation(tonemapProgram_, "uLUTScale");
    tmULUTOffset_      = glGetUniformLocation(tonemapProgram_, "uLUTOffset");

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
    const int tw = targetWidth();
    const int th = targetHeight();
    if (tonemapFBO_ && tonemapFBOWidth_ == tw && tonemapFBOHeight_ == th) return;

    destroyTonemapFBO();

    tonemapFBOWidth_  = tw;
    tonemapFBOHeight_ = th;

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

    // Reuse the mesh FBO's depth-stencil texture so the post-tonemap unlit
    // overlay pass can depth-test against the scene geometry that was
    // rendered there (with MSAA on, the resolved depth — see render3D).
    if (meshDepthTex_) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, meshDepthTex_, 0);
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
    // Read the same texture the compositor shows this frame: FXAA output
    // when that pass ran (always last), else tilt-shift output, else the
    // raw tonemap output. Mirrors finalColorTex().
    GLuint readFBO;
    if (fxaaActive_ && fxaaFBO_) {
        readFBO = fxaaFBO_;
        outW = fxaaWidth_;
        outH = fxaaHeight_;
    } else if (tiltActive_ && postFBO_) {
        readFBO = postFBO_;
        outW = postWidth_;
        outH = postHeight_;
    } else {
        readFBO = tonemapFBO_;
        outW = tonemapFBOWidth_;
        outH = tonemapFBOHeight_;
    }
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

    // Depth-of-field first: it rewrites the HDR image, so bloom and the
    // tonemap draw consume its output when it ran.
    const bool haveDoF = runDoFPass();
    const GLuint hdrSrc = haveDoF ? dofColorTex_ : meshColorTex_;

    // Bright-pass + blur the HDR source before resolving, so the tonemap
    // draw can add the glow in HDR. Leaves bloomActive_/bloomTex_ ready.
    const bool haveBloom = runBloomPrePass(hdrSrc);

    // Half-res AO from the resolved scene depth; multiplied into the lit
    // HDR image by the tonemap draw below (post-multiply — standard for a
    // forward renderer).
    const bool haveSSAO = runSSAOPass();
    ensureFallbackTextures();

    glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);
    glViewport(0, 0, tonemapFBOWidth_, tonemapFBOHeight_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(tonemapProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrSrc);
    glUniform1i(tmUTex_, 0);
    // Bloom on unit 1 — bind a valid texture even when off (intensity 0).
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, haveBloom ? bloomTex_[0] : hdrSrc);
    glUniform1i(tmUBloomTex_, 1);
    glUniform1f(tmUBloomIntensity_, haveBloom ? bloomIntensity_ : 0.0f);
    // SSAO on unit 2 — white fallback keeps the sampler valid when off.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, haveSSAO ? ssaoTex_[0] : fallback2D_);
    glUniform1i(tmUSSAOTex_, 2);
    glUniform1f(tmUSSAOIntensity_, haveSSAO ? ssaoIntensity_ : 0.0f);
    // Color LUT on unit 3 — 1x1x1 white fallback keeps the sampler3D valid
    // (cross-type aliasing on unit 0 is illegal in strict core profiles).
    const bool haveLUT = lutTex_ != 0 && lutAmount_ > 0.0f;
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, haveLUT ? lutTex_ : fallback3D_);
    glUniform1i(tmULUTTex_, 3);
    glUniform1f(tmULUTAmount_, haveLUT ? lutAmount_ : 0.0f);
    const float lutN = haveLUT ? static_cast<float>(lutSize_) : 1.0f;
    glUniform1f(tmULUTScale_, (lutN - 1.0f) / lutN);
    glUniform1f(tmULUTOffset_, 0.5f / lutN);
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

    bloomBrightProgram_ = linkProgram(kPostVertSrc, kBloomBrightFragSrc, "Bloom bright-pass");
    if (bloomBrightProgram_) {
        bbpUTex_       = glGetUniformLocation(bloomBrightProgram_, "uTex");
        bbpUThreshold_ = glGetUniformLocation(bloomBrightProgram_, "uThreshold");
    }
}

void SceneRenderer::ensureBloomFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int hw = std::max(1, targetWidth() / 2);
    const int hh = std::max(1, targetHeight() / 2);
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

bool SceneRenderer::runBloomPrePass(GLuint srcTex) {
    bloomActive_ = false;
    if (!bloomEnabled_ || bloomIntensity_ <= 0.0f || !srcTex) return false;

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

    // Bright-pass: HDR source → bloomTex_[0].
    glUseProgram(bloomBrightProgram_);
    glUniform1i(bbpUTex_, 0);
    glUniform1f(bbpUThreshold_, bloomThreshold_);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, srcTex);
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
// 3D color-grading LUT
// ---------------------------------------------------------------------------

bool SceneRenderer::loadColorLUT(const std::string& path, int size,
                                 float amount) {
    broimage::Image img;
    if (!broimage::decode_file(path, img) || img.width <= 0 || img.height <= 0) {
        LOG_ERROR("loadColorLUT: failed to decode '%s'", path.c_str());
        return false;
    }
    int n = size;
    if (n <= 0) n = img.height;   // infer cube side from strip height
    if (n < 2 || img.height != n || img.width != n * n) {
        LOG_ERROR("loadColorLUT: '%s' is %dx%d, expected a %dx%d strip "
                  "(size^2 x size, size=%d)",
                  path.c_str(), img.width, img.height, n * n, n, n);
        return false;
    }

    // Repack the horizontal strip (tile index = blue, tile x = red,
    // tile y = green, image rows top-down) into 3D-texture order:
    // x (red) fastest, then y (green), then z (blue).
    const int ch = img.channels;
    std::vector<uint8_t> vox(static_cast<size_t>(n) * n * n * 4, 255);
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) {
                const size_t src = (static_cast<size_t>(g) * img.width +
                                    static_cast<size_t>(b) * n + r) * ch;
                const size_t dst = ((static_cast<size_t>(b) * n + g) *
                                    static_cast<size_t>(n) + r) * 4;
                vox[dst + 0] = img.pixels[src + 0];
                vox[dst + 1] = ch > 1 ? img.pixels[src + 1] : img.pixels[src];
                vox[dst + 2] = ch > 2 ? img.pixels[src + 2] : img.pixels[src];
            }
        }
    }

    clearColorLUT();
    lutSize_   = n;
    lutAmount_ = amount < 0.0f ? 0.0f : amount;
    glGenTextures(1, &lutTex_);
    glBindTexture(GL_TEXTURE_3D, lutTex_);
    GLint prevUnpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, vox.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);
    return true;
}

void SceneRenderer::clearColorLUT() {
    if (lutTex_) { glDeleteTextures(1, &lutTex_); lutTex_ = 0; }
    lutSize_ = 0;
}

// ---------------------------------------------------------------------------
// SSAO pre-pass (half-res AO from resolved depth)
// ---------------------------------------------------------------------------

void SceneRenderer::ensureSSAOPipeline() {
    ensureTiltShiftPipeline();   // shares the separable-blur program
    if (ssaoProgram_) return;

    ssaoProgram_ = linkProgram(kPostVertSrc, kSSAOFragSrc, "SSAO program");
    if (!ssaoProgram_) return;

    aoUDepth_      = glGetUniformLocation(ssaoProgram_, "uDepthTex");
    aoUNoise_      = glGetUniformLocation(ssaoProgram_, "uNoiseTex");
    aoUProj_       = glGetUniformLocation(ssaoProgram_, "uProj");
    aoUInvProj_    = glGetUniformLocation(ssaoProgram_, "uInvProj");
    aoUKernel_     = glGetUniformLocation(ssaoProgram_, "uKernel");
    aoURadius_     = glGetUniformLocation(ssaoProgram_, "uRadius");
    aoUBias_       = glGetUniformLocation(ssaoProgram_, "uBias");
    aoUNoiseScale_ = glGetUniformLocation(ssaoProgram_, "uNoiseScale");

    // Deterministic LCG so the kernel/noise (and therefore AO output) are
    // identical across runs and machines.
    uint32_t seed = 0x9e3779b9u;
    auto frand = [&seed]() {   // [0,1)
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    };

    // Hemisphere kernel: unit vectors with z >= 0 (tangent space, +z along
    // the surface normal), pulled toward the origin so samples cluster near
    // the fragment (scale = lerp(0.1, 1, t^2)).
    for (int i = 0; i < 16; ++i) {
        float x = frand() * 2.0f - 1.0f;
        float y = frand() * 2.0f - 1.0f;
        float z = frand();
        float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-4f) { x = 0.0f; y = 0.0f; z = 1.0f; len = 1.0f; }
        float t = static_cast<float>(i) / 16.0f;
        float scale = (0.1f + 0.9f * t * t) * frand();
        ssaoKernel_[i * 3 + 0] = x / len * scale;
        ssaoKernel_[i * 3 + 1] = y / len * scale;
        ssaoKernel_[i * 3 + 2] = z / len * scale;
    }

    // 4x4 tiling rotation noise: random unit-ish xy per texel, packed into
    // RG8 as [0,1] (the shader expands back to [-1,1]).
    uint8_t noise[16 * 2];
    for (int i = 0; i < 16; ++i) {
        noise[i * 2 + 0] = static_cast<uint8_t>(frand() * 255.0f);
        noise[i * 2 + 1] = static_cast<uint8_t>(frand() * 255.0f);
    }
    glGenTextures(1, &ssaoNoiseTex_);
    glBindTexture(GL_TEXTURE_2D, ssaoNoiseTex_);
    GLint prevUnpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 4, 4, 0, GL_RG, GL_UNSIGNED_BYTE,
                 noise);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SceneRenderer::ensureSSAOFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int hw = std::max(1, targetWidth() / 2);
    const int hh = std::max(1, targetHeight() / 2);
    if (ssaoFBO_[0] && ssaoWidth_ == hw && ssaoHeight_ == hh) return;
    destroySSAOFBOs();

    ssaoWidth_  = hw;
    ssaoHeight_ = hh;
    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &ssaoFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_[i]);
        glGenTextures(1, &ssaoTex_[i]);
        glBindTexture(GL_TEXTURE_2D, ssaoTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, hw, hh, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, ssaoTex_[i], 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("SSAO FBO %d incomplete: 0x%x", i, status);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroySSAOFBOs() {
    for (int i = 0; i < 2; ++i) {
        if (ssaoTex_[i]) { glDeleteTextures(1, &ssaoTex_[i]); ssaoTex_[i] = 0; }
        if (ssaoFBO_[i]) { glDeleteFramebuffers(1, &ssaoFBO_[i]); ssaoFBO_[i] = 0; }
    }
    ssaoWidth_ = ssaoHeight_ = 0;
}

bool SceneRenderer::runSSAOPass() {
    if (!ssaoEnabled_ || ssaoIntensity_ <= 0.0f || !meshDepthTex_) return false;

    ensureSSAOPipeline();
    ensureSSAOFBOs();
    if (!ssaoProgram_ || !blurProgram_ || !ssaoFBO_[0]) return false;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, ssaoWidth_, ssaoHeight_);
    glBindVertexArray(tonemapVAO_);

    // AO estimate: resolved depth -> ssaoTex_[0].
    glUseProgram(ssaoProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, meshDepthTex_);
    glUniform1i(aoUDepth_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ssaoNoiseTex_);
    glUniform1i(aoUNoise_, 1);
    glActiveTexture(GL_TEXTURE0);

    const Mat4& proj = graph_.projectionMatrix_;
    const Mat4 invProj = bromath::minverse(proj);
    glUniformMatrix4fv(aoUProj_, 1, GL_FALSE, proj.data);
    glUniformMatrix4fv(aoUInvProj_, 1, GL_FALSE, invProj.data);
    glUniform3fv(aoUKernel_, 16, ssaoKernel_);
    glUniform1f(aoURadius_, ssaoRadius_);
    glUniform1f(aoUBias_, ssaoBias_);
    glUniform2f(aoUNoiseScale_, ssaoWidth_ / 4.0f, ssaoHeight_ / 4.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Separable blur (shared 9-tap Gaussian): erases the 4x4 noise pattern.
    // [0] -H-> [1] -V-> [0].
    glUseProgram(blurProgram_);
    glUniform1i(blUTex_, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_[1]);
    glBindTexture(GL_TEXTURE_2D, ssaoTex_[0]);
    glUniform2f(blUDir_, 1.0f / static_cast<float>(ssaoWidth_), 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, ssaoTex_[1]);
    glUniform2f(blUDir_, 0.0f, 1.0f / static_cast<float>(ssaoHeight_));
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// ---------------------------------------------------------------------------
// FXAA post pass (LDR, always last)
// ---------------------------------------------------------------------------

void SceneRenderer::ensureFXAAPipeline() {
    if (fxaaProgram_) return;
    fxaaProgram_ = linkProgram(kPostVertSrc, kFXAAFragSrc, "FXAA program");
    if (!fxaaProgram_) return;
    fxUTex_       = glGetUniformLocation(fxaaProgram_, "uTex");
    fxUTexelSize_ = glGetUniformLocation(fxaaProgram_, "uTexelSize");
}

void SceneRenderer::ensureFXAAFBO() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int tw = targetWidth();
    const int th = targetHeight();
    if (fxaaFBO_ && fxaaWidth_ == tw && fxaaHeight_ == th) return;
    destroyFXAAFBO();

    fxaaWidth_  = tw;
    fxaaHeight_ = th;
    glGenFramebuffers(1, &fxaaFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, fxaaFBO_);
    glGenTextures(1, &fxaaColorTex_);
    glBindTexture(GL_TEXTURE_2D, fxaaColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           fxaaColorTex_, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("FXAA FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyFXAAFBO() {
    if (fxaaColorTex_) { glDeleteTextures(1, &fxaaColorTex_); fxaaColorTex_ = 0; }
    if (fxaaFBO_)      { glDeleteFramebuffers(1, &fxaaFBO_); fxaaFBO_ = 0; }
    fxaaWidth_ = fxaaHeight_ = 0;
}

void SceneRenderer::runFXAAPass() {
    fxaaActive_ = false;
    if (!fxaaEnabled_) return;

    // Input = whatever the compositor would otherwise show (tilt output
    // when that pass ran, else the tonemap output).
    const GLuint srcTex = (tiltActive_ && postColorTex_) ? postColorTex_
                                                         : tonemapColorTex_;
    if (!srcTex) return;

    ensureFXAAPipeline();
    ensureFXAAFBO();
    if (!fxaaProgram_ || !fxaaFBO_) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindFramebuffer(GL_FRAMEBUFFER, fxaaFBO_);
    glViewport(0, 0, fxaaWidth_, fxaaHeight_);
    glUseProgram(fxaaProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(fxUTex_, 0);
    glUniform2f(fxUTexelSize_, 1.0f / static_cast<float>(fxaaWidth_),
                1.0f / static_cast<float>(fxaaHeight_));

    glBindVertexArray(tonemapVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fxaaActive_ = true;
}

// ---------------------------------------------------------------------------
// Depth-of-field pre-pass (HDR, before bloom + tonemap)
// ---------------------------------------------------------------------------

void SceneRenderer::ensureDoFPipeline() {
    ensureTiltShiftPipeline();   // shares the separable-blur program
    if (dofProgram_) return;

    dofProgram_ = linkProgram(kPostVertSrc, kDoFCompositeFragSrc, "DoF composite");
    if (!dofProgram_) return;

    dofUSharp_         = glGetUniformLocation(dofProgram_, "uSharp");
    dofUBlur_          = glGetUniformLocation(dofProgram_, "uBlur");
    dofUDepth_         = glGetUniformLocation(dofProgram_, "uDepthTex");
    dofUDepthRange_    = glGetUniformLocation(dofProgram_, "uDepthRange");
    dofUPerspective_   = glGetUniformLocation(dofProgram_, "uPerspective");
    dofUFocusDistance_ = glGetUniformLocation(dofProgram_, "uFocusDistance");
    dofUFocusRange_    = glGetUniformLocation(dofProgram_, "uFocusRange");
}

void SceneRenderer::ensureDoFFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int tw = targetWidth();
    const int th = targetHeight();
    const int hw = std::max(1, tw / 2);
    const int hh = std::max(1, th / 2);
    if (dofFBO_ && dofWidth_ == tw && dofHeight_ == th &&
        dofBlurFBO_[0] && dofBlurWidth_ == hw && dofBlurHeight_ == hh) {
        return;
    }
    destroyDoFFBOs();

    // Everything HDR (RGBA16F): DoF runs before tonemap, and the blurred
    // image must carry >1.0 highlights through to bloom.
    dofBlurWidth_  = hw;
    dofBlurHeight_ = hh;
    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &dofBlurFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, dofBlurFBO_[i]);
        glGenTextures(1, &dofBlurTex_[i]);
        glBindTexture(GL_TEXTURE_2D, dofBlurTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, dofBlurTex_[i], 0);
    }

    dofWidth_  = tw;
    dofHeight_ = th;
    glGenFramebuffers(1, &dofFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, dofFBO_);
    glGenTextures(1, &dofColorTex_);
    glBindTexture(GL_TEXTURE_2D, dofColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tw, th, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           dofColorTex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("DoF FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyDoFFBOs() {
    for (int i = 0; i < 2; ++i) {
        if (dofBlurTex_[i]) { glDeleteTextures(1, &dofBlurTex_[i]); dofBlurTex_[i] = 0; }
        if (dofBlurFBO_[i]) { glDeleteFramebuffers(1, &dofBlurFBO_[i]); dofBlurFBO_[i] = 0; }
    }
    if (dofColorTex_) { glDeleteTextures(1, &dofColorTex_); dofColorTex_ = 0; }
    if (dofFBO_)      { glDeleteFramebuffers(1, &dofFBO_); dofFBO_ = 0; }
    dofBlurWidth_ = dofBlurHeight_ = 0;
    dofWidth_ = dofHeight_ = 0;
}

bool SceneRenderer::runDoFPass() {
    if (!dofEnabled_ || !meshColorTex_ || !meshDepthTex_) return false;

    ensureDoFPipeline();
    ensureDoFFBOs();
    if (!dofProgram_ || !blurProgram_ || !dofFBO_ || !dofBlurFBO_[0]) return false;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(tonemapVAO_);

    // --- Downsample + separable Gaussian (HDR sharp -> dofBlurTex_[1]) -----
    const float rx = dofMaxBlur_ / static_cast<float>(dofBlurWidth_);
    const float ry = dofMaxBlur_ / static_cast<float>(dofBlurHeight_);
    glUseProgram(blurProgram_);
    glUniform1i(blUTex_, 0);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, dofBlurWidth_, dofBlurHeight_);

    glBindFramebuffer(GL_FRAMEBUFFER, dofBlurFBO_[0]);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glUniform2f(blUDir_, rx, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, dofBlurFBO_[1]);
    glBindTexture(GL_TEXTURE_2D, dofBlurTex_[0]);
    glUniform2f(blUDir_, 0.0f, ry);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // --- CoC composite (sharp + blur + depth -> dofColorTex_) --------------
    glUseProgram(dofProgram_);
    glBindFramebuffer(GL_FRAMEBUFFER, dofFBO_);
    glViewport(0, 0, dofWidth_, dofHeight_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glUniform1i(dofUSharp_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, dofBlurTex_[1]);
    glUniform1i(dofUBlur_, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, meshDepthTex_);
    glUniform1i(dofUDepth_, 2);
    glActiveTexture(GL_TEXTURE0);
    glUniform2f(dofUDepthRange_, graph_.cameraNearZ_, graph_.cameraFarZ_);
    glUniform1i(dofUPerspective_, graph_.cameraIsPerspective_ ? 1 : 0);
    glUniform1f(dofUFocusDistance_, dofFocusDistance_);
    glUniform1f(dofUFocusRange_, dofFocusRange_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Tilt-shift DOF post pass
// ---------------------------------------------------------------------------

void SceneRenderer::ensureTiltShiftPipeline() {
    if (blurProgram_ && tiltProgram_) return;

    if (!blurProgram_) {
        blurProgram_ = linkProgram(kPostVertSrc, kBlurFragSrc, "Blur program");
        if (blurProgram_) {
            blUTex_ = glGetUniformLocation(blurProgram_, "uTex");
            blUDir_ = glGetUniformLocation(blurProgram_, "uDir");
        }
    }

    if (!tiltProgram_) {
        tiltProgram_ = linkProgram(kPostVertSrc, kTiltCompositeFragSrc, "Tilt-shift program");
        if (tiltProgram_) {
            tsUSharp_       = glGetUniformLocation(tiltProgram_, "uSharp");
            tsUBlur_        = glGetUniformLocation(tiltProgram_, "uBlur");
            tsUFocusCenter_ = glGetUniformLocation(tiltProgram_, "uFocusCenter");
            tsUFocusWidth_  = glGetUniformLocation(tiltProgram_, "uFocusWidth");
            tsUFeather_     = glGetUniformLocation(tiltProgram_, "uFeather");
            tsUSaturation_  = glGetUniformLocation(tiltProgram_, "uSaturation");
            tsUContrast_    = glGetUniformLocation(tiltProgram_, "uContrast");
        }
    }
}

void SceneRenderer::ensureTiltShiftFBOs() {
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;

    const int tw = targetWidth();
    const int th = targetHeight();
    const int hw = std::max(1, tw / 2);
    const int hh = std::max(1, th / 2);

    if (blurFBO_[0] && blurWidth_ == hw && blurHeight_ == hh &&
        postFBO_ && postWidth_ == tw && postHeight_ == th) {
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
    postWidth_  = tw;
    postHeight_ = th;
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
