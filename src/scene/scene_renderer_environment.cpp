#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/atmosphere_irradiance.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bro::scene {

using bromath::Vec3;

void SceneRenderer::ensureEnvConvertPipeline() {
}

bool SceneRenderer::runEquirectToCubemap(GLuint /*equirectTex*/, GLuint /*cubemap*/, int /*faceSize*/) {
    return false;
}

bool SceneRenderer::loadEnvironment(const std::string& hdrPath) {
    if (hdrPath.empty()) {
        clearEnvironment();
        return true;
    }
    return true;
}

void SceneRenderer::clearEnvironment() {
    envCubemap_ = 0;
    envIrradianceCube_ = 0;
    envPrefilterCube_ = 0;
}

void SceneRenderer::ensureBRDFLUT() {
}

void SceneRenderer::ensureIrradiancePipeline() {
}

void SceneRenderer::ensureAtmospherePipeline() {
}

void SceneRenderer::uploadAtmosphereUniforms(GLuint /*prog*/) {
}

void SceneRenderer::resolveAtmLocs(GLuint /*prog*/, AtmLocs& /*a*/) const {
}

void SceneRenderer::uploadAtmLocs(const AtmLocs& /*L*/) const {
}

void SceneRenderer::renderAtmospherePass() {
}

void SceneRenderer::ensureSkyboxPipeline() {
}

void SceneRenderer::renderSkyboxPass() {
}

void SceneRenderer::ensureStarfieldPipeline() {
}

void SceneRenderer::renderStarfieldPass() {
}

void SceneRenderer::ensurePrefilterPipeline() {
}

bool SceneRenderer::runPrefilterInto(GLuint /*srcCube*/, int /*srcSize*/,
                                     GLuint /*dstCube*/, int /*dstSize*/, int /*mips*/) {
    return false;
}

void SceneRenderer::updateSunIrradiance(const std::vector<LightNode*>& lights) {
    const LightNode* best = nullptr;
    float bestPower = -1.0f;
    for (const LightNode* l : lights) {
        if (!l || l->kind() != LightNode::Kind::Directional) continue;
        const bromath::Vec3& c = l->color();
        const float power = (c.x + c.y + c.z) * (1.0f / 3.0f) * l->intensity();
        if (power > bestPower) { bestPower = power; best = l; }
    }
    if (!best) return;
    const bromath::Vec3& c = best->color();
    sunIrradiance_[0] = c.x * best->intensity();
    sunIrradiance_[1] = c.y * best->intensity();
    sunIrradiance_[2] = c.z * best->intensity();
}

void SceneRenderer::updateSkyAmbient(float camY) {
    if (!atmosphere_.enabled) return;
    if (std::abs(camY - skyAmbientCamY_) < 25.0f) return;
    skyAmbientCamY_ = camY;
    AtmosphereParams a = atmosphere_;
    const float* sun = effectiveSunColor();
    a.sunColor[0] = sun[0]; a.sunColor[1] = sun[1]; a.sunColor[2] = sun[2];
    computeSkyAmbient(a, camY, skyAmbient_);
}

}  // namespace bro::scene
