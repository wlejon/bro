#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/vulkan/scene_vk_bridge.h"
#include "util/log.h"
#include "broimage/decode.h"

#include <algorithm>
#include <vector>

namespace bro::scene {

void SceneRenderer::ensureTonemapPipeline() {
}

void SceneRenderer::ensureTonemapFBO() {
}

void SceneRenderer::destroyTonemapFBO() {
    tonemapColorTex_ = 0;
    tonemapFBO_ = 0;
    tonemapFBOWidth_ = tonemapFBOHeight_ = 0;
}

void SceneRenderer::runTonemapPass() {
}

void SceneRenderer::ensureBloomPipeline() {
}

void SceneRenderer::ensureBloomFBOs() {
}

void SceneRenderer::destroyBloomFBOs() {
    for (int i = 0; i < 2; ++i) {
        bloomTex_[i] = 0;
        bloomFBO_[i] = 0;
    }
    bloomWidth_ = bloomHeight_ = 0;
}

bool SceneRenderer::loadColorLUT(const std::string& path, int size,
                                 float amount) {
    broimage::Image img;
    if (!broimage::decode_file(path, img) || img.width <= 0 || img.height <= 0) {
        LOG_ERROR("loadColorLUT: failed to decode '%s'", path.c_str());
        return false;
    }
    int n = size;
    if (n <= 0) n = img.height;
    if (n < 2 || img.height != n || img.width != n * n) {
        LOG_ERROR("loadColorLUT: '%s' is %dx%d, expected a %dx%d strip "
                  "(size^2 x size, size=%d)",
                  path.c_str(), img.width, img.height, n * n, n, n);
        return false;
    }

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
    lutVoxels_ = std::move(vox);
    lutDirty_  = true;
    return true;
}

void SceneRenderer::clearColorLUT() {
    lutTex_ = 0;
    lutSize_ = 0;
    lutVoxels_.clear();
    lutDirty_ = true;
}

void SceneRenderer::ensureSSAOPipeline() {
}

void SceneRenderer::ensureSSAOFBOs() {
}

void SceneRenderer::destroySSAOFBOs() {
    for (int i = 0; i < 2; ++i) {
        ssaoTex_[i] = 0;
        ssaoFBO_[i] = 0;
    }
    ssaoWidth_ = ssaoHeight_ = 0;
}

void SceneRenderer::ensureFXAAPipeline() {
}

void SceneRenderer::ensureFXAAFBO() {
}

void SceneRenderer::destroyFXAAFBO() {
    fxaaColorTex_ = 0;
    fxaaFBO_ = 0;
    fxaaWidth_ = fxaaHeight_ = 0;
}

void SceneRenderer::runFXAAPass() {
}

void SceneRenderer::ensureDoFPipeline() {
}

void SceneRenderer::ensureDoFFBOs() {
}

void SceneRenderer::destroyDoFFBOs() {
    for (int i = 0; i < 2; ++i) {
        dofBlurTex_[i] = 0;
        dofBlurFBO_[i] = 0;
    }
    dofColorTex_ = 0;
    dofFBO_ = 0;
    dofWidth_ = dofHeight_ = dofBlurWidth_ = dofBlurHeight_ = 0;
}

void SceneRenderer::ensureTiltShiftPipeline() {
}

void SceneRenderer::ensureTiltShiftFBOs() {
}

void SceneRenderer::destroyTiltShiftFBOs() {
    for (int i = 0; i < 2; ++i) {
        blurTex_[i] = 0;
        blurFBO_[i] = 0;
    }
    postColorTex_ = 0;
    postFBO_ = 0;
    postWidth_ = postHeight_ = blurWidth_ = blurHeight_ = 0;
}

void SceneRenderer::runTiltShiftPass() {
}

std::vector<uint8_t> SceneRenderer::readTonemapPixelsRGBA(int& outW, int& outH) {
    if (vkBridge_) {
        return vkBridge_->readTonemapPixelsRGBA(outW, outH);
    }
    outW = 0;
    outH = 0;
    return {};
}

}  // namespace bro::scene
