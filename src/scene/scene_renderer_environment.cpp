#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/atmosphere_irradiance.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "env_convert.vert.h"
#include "env_convert.frag.h"
#include "irradiance.frag.h"
#include "prefilter.frag.h"
#include "brdf_lut.frag.h"
#include "skybox.vert.h"
#include "skybox.frag.h"
#include "atmosphere.glsl.h"      // kAtmosphereSrc
#include "sky_atmosphere.frag.h"  // kSkyAtmosphereFragSrc

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// IBL: HDR equirect → cubemap conversion. The cubemap produced here is the
// raw radiance source; later passes (irradiance convolution, prefiltered
// specular) consume it to populate the IBL data the PBR shader samples.
// ---------------------------------------------------------------------------

void SceneRenderer::ensureEnvConvertPipeline() {
    if (envConvertProgram_) return;

    envConvertProgram_ = linkProgram(kEnvConvertVertSrc, kEnvConvertFragSrc, "Env convert program");
    envCvUFace_     = glGetUniformLocation(envConvertProgram_, "uFace");
    envCvUEquirect_ = glGetUniformLocation(envConvertProgram_, "uEquirect");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &envConvertVAO_);
    glGenBuffers(1, &envConvertVBO_);
    glBindVertexArray(envConvertVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, envConvertVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &envConvertFBO_);
}

bool SceneRenderer::runEquirectToCubemap(GLuint equirectTex, GLuint cubemap, int faceSize) {
    ensureEnvConvertPipeline();
    if (!envConvertProgram_ || !envConvertFBO_) return false;

    // Save state we touch so the caller's render flow isn't disturbed.
    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, envConvertFBO_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, faceSize, faceSize);

    glUseProgram(envConvertProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, equirectTex);
    glUniform1i(envCvUEquirect_, 0);
    glBindVertexArray(envConvertVAO_);

    bool ok = true;
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemap, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Env convert FBO incomplete on face %d: 0x%x", face, status);
            ok = false;
            break;
        }
        glUniform1i(envCvUFace_, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return ok;
}

bool SceneRenderer::loadEnvironment(const std::string& hdrPath) {
    if (hdrPath.empty()) {
        clearEnvironment();
        return true;
    }

    // broimage::decode_file_f32 returns top-down float RGBA (4 channels — the
    // alpha is unused by the cubemap conv shader, which samples .rgb). The
    // shader's UV mapping (`0.5 - theta/PI`) is paired with this orientation.
    broimage::ImageF32 hdr;
    std::string err;
    if (!broimage::decode_file_f32(hdrPath, hdr, &err)) {
        LOG_ERROR("loadEnvironment: decode_file_f32 failed for '%s': %s",
                  hdrPath.c_str(), err.c_str());
        return false;
    }
    const int w = hdr.width;
    const int h = hdr.height;

    // Upload the equirect as a temp 2D float texture.
    GLuint equirectTex = 0;
    glGenTextures(1, &equirectTex);
    glBindTexture(GL_TEXTURE_2D, equirectTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, hdr.pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // (Re)allocate the destination cubemap. 1024² per face matches a 4k
    // equirect's angular density (~11 texels/deg) so we don't downsample
    // good source HDRIs. Mip chain is reserved upfront for trilinear
    // skybox sampling and to give glGenerateMipmap somewhere to write.
    const int faceSize = 1024;
    if (envCubemap_ && envCubemapSize_ != faceSize) {
        glDeleteTextures(1, &envCubemap_);
        envCubemap_ = 0;
    }
    if (!envCubemap_) {
        glGenTextures(1, &envCubemap_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
        for (int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                         faceSize, faceSize, 0, GL_RGBA, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        envCubemapSize_ = faceSize;
    }

    bool ok = runEquirectToCubemap(equirectTex, envCubemap_, faceSize);
    glDeleteTextures(1, &equirectTex);
    if (!ok) {
        // Don't keep a half-baked cubemap.
        glDeleteTextures(1, &envCubemap_);
        envCubemap_ = 0;
        envCubemapSize_ = 0;
        envPath_.clear();
        return false;
    }

    // Generate mips so trilinear sampling at low LOD looks clean (and so
    // the prefilter pass has somewhere to write its roughness chain).
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    envPath_ = hdrPath;

    // Build the IBL precomputed maps from the freshly populated env cube.
    // Both are slow (~100M+ texture taps each) but one-shot per HDR load;
    // the runtime loop only samples the small results.
    if (!runIrradianceConvolution()) {
        LOG_WARN("Loaded environment '%s' but irradiance convolution failed",
                 hdrPath.c_str());
    }
    if (!runPrefilterConvolution()) {
        LOG_WARN("Loaded environment '%s' but prefilter convolution failed",
                 hdrPath.c_str());
    }
    // BRDF LUT is env-independent; bake it once on the first env load.
    ensureBRDFLUT();

    LOG_INFO("Loaded HDR environment '%s' (%dx%d → cube %d², irradiance %d², prefilter %d² × %d mips)",
             hdrPath.c_str(), w, h, faceSize, envIrradianceSize_,
             envPrefilterSize_, envPrefilterMips_);
    return true;
}

void SceneRenderer::clearEnvironment() {
    if (envCubemap_) { glDeleteTextures(1, &envCubemap_); envCubemap_ = 0; }
    if (envIrradianceCube_) { glDeleteTextures(1, &envIrradianceCube_); envIrradianceCube_ = 0; }
    if (envPrefilterCube_) { glDeleteTextures(1, &envPrefilterCube_); envPrefilterCube_ = 0; }
    // brdfLUT_ is env-independent; intentionally NOT freed here.
    envCubemapSize_ = 0;
    envPath_.clear();
}

void SceneRenderer::ensureBRDFLUT() {
    if (brdfLUT_) return;
    ensureEnvConvertPipeline();   // shared NDC quad + FBO

    if (!brdfLUTProgram_) {
        brdfLUTProgram_ = linkProgram(kEnvConvertVertSrc, kBRDFLUTFragSrc, "BRDF LUT program");
    }

    glGenTextures(1, &brdfLUT_);
    glBindTexture(GL_TEXTURE_2D, brdfLUT_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, brdfLUTSize_, brdfLUTSize_, 0,
                 GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, envConvertFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           brdfLUT_, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("BRDF LUT FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
        glDeleteTextures(1, &brdfLUT_);
        brdfLUT_ = 0;
        return;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, brdfLUTSize_, brdfLUTSize_);

    glUseProgram(brdfLUTProgram_);
    glBindVertexArray(envConvertVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

void SceneRenderer::ensureIrradiancePipeline() {
    if (irrConvProgram_) return;
    irrConvProgram_ = linkProgram(kEnvConvertVertSrc, kIrradianceFragSrc, "Irradiance program");
    irrCvUEnv_  = glGetUniformLocation(irrConvProgram_, "uEnv");
    irrCvUFace_ = glGetUniformLocation(irrConvProgram_, "uFace");
}

bool SceneRenderer::runIrradianceConvolution() {
    if (!envCubemap_) return false;
    ensureEnvConvertPipeline();   // we reuse its FBO + VAO
    ensureIrradiancePipeline();
    if (!irrConvProgram_ || !envConvertFBO_) return false;

    const int faceSize = envIrradianceSize_;
    if (!envIrradianceCube_) {
        glGenTextures(1, &envIrradianceCube_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, envIrradianceCube_);
        for (int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                         faceSize, faceSize, 0, GL_RGBA, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, envConvertFBO_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, faceSize, faceSize);

    glUseProgram(irrConvProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
    glUniform1i(irrCvUEnv_, 0);
    glBindVertexArray(envConvertVAO_);

    bool ok = true;
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                               envIrradianceCube_, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Irradiance FBO incomplete on face %d: 0x%x", face, status);
            ok = false;
            break;
        }
        glUniform1i(irrCvUFace_, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return ok;
}

// ---------------------------------------------------------------------------
// Analytic sky
// ---------------------------------------------------------------------------

void SceneRenderer::ensureAtmospherePipeline() {
    if (atmProgram_) return;
    ensureSkyboxPipeline();          // shares its vertex shader, VAO and quad
    if (!skyboxVAO_) return;

    // atmosphere.glsl is a bare chunk with no #version of its own, so it goes
    // in after the sky shader's. Same splice the mesh shaders use for their
    // feature defines.
    const std::string frag =
        insertAfterVersion(kSkyAtmosphereFragSrc, std::string(kAtmosphereSrc) + "\n");
    atmProgram_ = linkProgram(kSkyboxVertSrc, frag.c_str(), "Atmosphere sky program");
    if (!atmProgram_) return;

    atmUViewToWorld_      = glGetUniformLocation(atmProgram_, "uViewToWorld");
    atmUTanHalfFovY_      = glGetUniformLocation(atmProgram_, "uTanHalfFovY");
    atmUAspect_           = glGetUniformLocation(atmProgram_, "uAspect");
    atmUCamPos_           = glGetUniformLocation(atmProgram_, "uCamPos");
    atmUSunAngularRadius_ = glGetUniformLocation(atmProgram_, "uSunAngularRadius");
    atmUSunDiskIntensity_ = glGetUniformLocation(atmProgram_, "uSunDiskIntensity");
}

// Shared by the sky pass and (later) by anything else that integrates the same
// model, so the parameters cannot drift between them.
void SceneRenderer::uploadAtmosphereUniforms(GLuint prog) {
    const AtmosphereParams& a = atmosphere_;

    float len = std::sqrt(a.sunDir[0] * a.sunDir[0] + a.sunDir[1] * a.sunDir[1]
                        + a.sunDir[2] * a.sunDir[2]);
    if (!(len > 0.0f)) len = 1.0f;
    const float sd[3] = {a.sunDir[0] / len, a.sunDir[1] / len, a.sunDir[2] / len};

    auto u3 = [&](const char* n, const float* v) {
        const GLint l = glGetUniformLocation(prog, n);
        if (l >= 0) glUniform3fv(l, 1, v);
    };
    auto u1 = [&](const char* n, float v) {
        const GLint l = glGetUniformLocation(prog, n);
        if (l >= 0) glUniform1f(l, v);
    };
    u3("uAtmSunDir", sd);
    u3("uAtmSunColor", a.sunColor);
    u3("uAtmBetaR", a.betaR);
    u1("uAtmPlanetRadius", a.planetRadius);
    u1("uAtmThickness", a.thickness);
    u1("uAtmBetaM", a.betaM);
    u1("uAtmMieG", a.mieG);
    u1("uAtmScaleHeightR", a.scaleHeightR);
    u1("uAtmScaleHeightM", a.scaleHeightM);
    u1("uAtmSeaLevel", a.seaLevel);
}

void SceneRenderer::resolveAtmLocs(GLuint prog, AtmLocs& a) const {
    auto U = [&](const char* n) { return glGetUniformLocation(prog, n); };
    a.enabled      = U("uAtmEnabled");
    a.camPos       = U("uAtmCamPos");
    a.sunDir       = U("uAtmSunDir");
    a.sunColor     = U("uAtmSunColor");
    a.betaR        = U("uAtmBetaR");
    a.planetRadius = U("uAtmPlanetRadius");
    a.thickness    = U("uAtmThickness");
    a.betaM        = U("uAtmBetaM");
    a.mieG         = U("uAtmMieG");
    a.scaleHeightR = U("uAtmScaleHeightR");
    a.scaleHeightM = U("uAtmScaleHeightM");
    a.seaLevel     = U("uAtmSeaLevel");
}

void SceneRenderer::uploadAtmLocs(const AtmLocs& L) const {
    const AtmosphereParams& a = atmosphere_;
    if (L.enabled >= 0) glUniform1i(L.enabled, a.enabled ? 1 : 0);
    if (!a.enabled) return;   // the rest is dead weight when the branch is off

    float len = std::sqrt(a.sunDir[0] * a.sunDir[0] + a.sunDir[1] * a.sunDir[1]
                        + a.sunDir[2] * a.sunDir[2]);
    if (!(len > 0.0f)) len = 1.0f;

    const bromath::Vec3& eye = graph_.cameraEye();
    if (L.camPos >= 0) glUniform3f(L.camPos, eye.x, eye.y, eye.z);
    if (L.sunDir >= 0)
        glUniform3f(L.sunDir, a.sunDir[0] / len, a.sunDir[1] / len, a.sunDir[2] / len);
    if (L.sunColor     >= 0) glUniform3fv(L.sunColor, 1, a.sunColor);
    if (L.betaR        >= 0) glUniform3fv(L.betaR, 1, a.betaR);
    if (L.planetRadius >= 0) glUniform1f(L.planetRadius, a.planetRadius);
    if (L.thickness    >= 0) glUniform1f(L.thickness, a.thickness);
    if (L.betaM        >= 0) glUniform1f(L.betaM, a.betaM);
    if (L.mieG         >= 0) glUniform1f(L.mieG, a.mieG);
    if (L.scaleHeightR >= 0) glUniform1f(L.scaleHeightR, a.scaleHeightR);
    if (L.scaleHeightM >= 0) glUniform1f(L.scaleHeightM, a.scaleHeightM);
    if (L.seaLevel     >= 0) glUniform1f(L.seaLevel, a.seaLevel);
}

void SceneRenderer::renderAtmospherePass() {
    if (!atmosphere_.enabled) return;
    if (!graph_.cameraIsPerspective_) return;   // ortho has no view direction
    ensureAtmospherePipeline();
    if (!atmProgram_) return;

    float viewToWorld[9] = {
        graph_.viewMatrix_.at(0, 0), graph_.viewMatrix_.at(1, 0), graph_.viewMatrix_.at(2, 0),
        graph_.viewMatrix_.at(0, 1), graph_.viewMatrix_.at(1, 1), graph_.viewMatrix_.at(2, 1),
        graph_.viewMatrix_.at(0, 2), graph_.viewMatrix_.at(1, 2), graph_.viewMatrix_.at(2, 2),
    };

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(atmProgram_);
    glUniformMatrix3fv(atmUViewToWorld_, 1, GL_FALSE, viewToWorld);
    glUniform1f(atmUTanHalfFovY_, std::tan(graph_.cameraFovY_ * 0.5f));
    glUniform1f(atmUAspect_, graph_.cameraAspect_);
    const bromath::Vec3& eye = graph_.cameraEye();
    glUniform3f(atmUCamPos_, eye.x, eye.y, eye.z);
    glUniform1f(atmUSunAngularRadius_, atmosphere_.sunAngularRadius);
    glUniform1f(atmUSunDiskIntensity_, atmosphere_.sunDiskIntensity);
    uploadAtmosphereUniforms(atmProgram_);

    glBindVertexArray(skyboxVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void SceneRenderer::ensureSkyboxPipeline() {
    if (skyboxProgram_) return;

    skyboxProgram_ = linkProgram(kSkyboxVertSrc, kSkyboxFragSrc, "Skybox program");
    skyUViewToWorld_ = glGetUniformLocation(skyboxProgram_, "uViewToWorld");
    skyUTanHalfFovY_ = glGetUniformLocation(skyboxProgram_, "uTanHalfFovY");
    skyUAspect_      = glGetUniformLocation(skyboxProgram_, "uAspect");
    skyUEnv_         = glGetUniformLocation(skyboxProgram_, "uEnv");
    skyUIntensity_   = glGetUniformLocation(skyboxProgram_, "uIntensity");
    skyURotation_    = glGetUniformLocation(skyboxProgram_, "uRotation");

    static const float quadVerts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &skyboxVAO_);
    glGenBuffers(1, &skyboxVBO_);
    glBindVertexArray(skyboxVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, (void*)0);
    glBindVertexArray(0);
}

void SceneRenderer::renderSkyboxPass() {
    if (!envCubemap_) return;
    if (!graph_.cameraIsPerspective_) return;  // Ortho cameras have no view direction.
    ensureSkyboxPipeline();
    if (!skyboxProgram_) return;

    // viewMatrix_ stores world→view (column-major). The 3x3 rotation block
    // is orthonormal (lookAt produces it), so its transpose is its inverse
    // and gives view→world. Pass that to the shader as a mat3.
    float viewToWorld[9] = {
        graph_.viewMatrix_.at(0, 0), graph_.viewMatrix_.at(1, 0), graph_.viewMatrix_.at(2, 0),
        graph_.viewMatrix_.at(0, 1), graph_.viewMatrix_.at(1, 1), graph_.viewMatrix_.at(2, 1),
        graph_.viewMatrix_.at(0, 2), graph_.viewMatrix_.at(1, 2), graph_.viewMatrix_.at(2, 2),
    };
    // GLSL mat3 columns are: column 0 = view→world basis vector for view-X.
    // viewMatrix's row 0 (m[0..2][0]) is the world-space camera-right vector,
    // which is exactly view-X→world. So packing rows-of-view as cols-of-m3
    // gives the transpose we want. The pack above does that.

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(skyboxProgram_);
    glUniformMatrix3fv(skyUViewToWorld_, 1, GL_FALSE, viewToWorld);
    glUniform1f(skyUTanHalfFovY_, std::tan(graph_.cameraFovY_ * 0.5f));
    glUniform1f(skyUAspect_, graph_.cameraAspect_);
    glUniform1f(skyUIntensity_, envIntensity_);
    glUniform1f(skyURotation_, envRotation_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap_);
    glUniform1i(skyUEnv_, 0);

    glBindVertexArray(skyboxVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    // Re-enable depth write/test for the geometry passes that follow.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void SceneRenderer::ensurePrefilterPipeline() {
    if (prefilterProgram_) return;
    prefilterProgram_ = linkProgram(kEnvConvertVertSrc, kPrefilterFragSrc, "Prefilter program");
    pfUEnv_       = glGetUniformLocation(prefilterProgram_, "uEnv");
    pfUFace_      = glGetUniformLocation(prefilterProgram_, "uFace");
    pfURoughness_ = glGetUniformLocation(prefilterProgram_, "uRoughness");
    pfUEnvSize_   = glGetUniformLocation(prefilterProgram_, "uEnvSize");
}

bool SceneRenderer::runPrefilterConvolution() {
    if (!envCubemap_) return false;

    if (!envPrefilterCube_) {
        glGenTextures(1, &envPrefilterCube_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, envPrefilterCube_);
        for (int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                         envPrefilterSize_, envPrefilterSize_, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Allocate the mip storage upfront so per-mip FBO attachment works.
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    }

    return runPrefilterInto(envCubemap_, envCubemapSize_, envPrefilterCube_,
                            envPrefilterSize_, envPrefilterMips_);
}

// Shared GGX prefilter core: convolve `srcCube` into `dstCube`'s roughness
// mip chain. Used by the global environment (above) and by per-probe
// captures (scene_renderer_probes.cpp). `dstCube` must already have its mip
// storage allocated; `srcCube` must be mipmapped (the Krivanek bias samples
// LODs of it).
bool SceneRenderer::runPrefilterInto(GLuint srcCube, int srcSize,
                                     GLuint dstCube, int dstSize, int mips) {
    if (!srcCube || !dstCube || mips < 1) return false;
    ensureEnvConvertPipeline();
    ensurePrefilterPipeline();
    if (!prefilterProgram_ || !envConvertFBO_) return false;

    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, envConvertFBO_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(prefilterProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, srcCube);
    glUniform1i(pfUEnv_, 0);
    glUniform1f(pfUEnvSize_, (float)srcSize);
    glBindVertexArray(envConvertVAO_);

    bool ok = true;
    for (int mip = 0; mip < mips && ok; ++mip) {
        int mipSize = dstSize >> mip;
        if (mipSize < 1) mipSize = 1;
        float roughness = (mips <= 1)
                          ? 0.0f
                          : (float)mip / (float)(mips - 1);
        glViewport(0, 0, mipSize, mipSize);
        glUniform1f(pfURoughness_, roughness);

        for (int face = 0; face < 6; ++face) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                                   dstCube, mip);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOG_ERROR("Prefilter FBO incomplete (mip %d face %d): 0x%x",
                          mip, face, status);
                ok = false;
                break;
            }
            glUniform1i(pfUFace_, face);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return ok;
}

// Refresh the cached sky ambient. Skipped entirely when the atmosphere is off,
// and when neither the altitude nor the parameters have moved — a static camera
// under a static sun recomputes nothing.
void SceneRenderer::updateSkyAmbient(float camY) {
    if (!atmosphere_.enabled) return;
    // Irradiance changes slowly with height; a metre of bob is not worth an
    // integration, but a climb into thin air very much is.
    if (std::abs(camY - skyAmbientCamY_) < 25.0f) return;
    skyAmbientCamY_ = camY;
    computeSkyAmbient(atmosphere_, camY, skyAmbient_);
}

}  // namespace bro::scene
