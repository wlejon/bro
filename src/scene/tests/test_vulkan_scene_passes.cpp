// bro_vulkan_scene_passes_test: the built-in SPIR-V shaders, run-time GLSL
// compilation, and the scene renderer's full pass list run on a real graph.
//
// assert() is the check here, so it must survive a Release build: NDEBUG is
// undefined before any header can pull in <cassert>. Run under
// BRO_VK_VALIDATION=1 (tests/run_tests.sh does) and any validation error
// fails the run too.
#undef NDEBUG

#include "scene/particles3d_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "util/log.h"

#include <broimage/encode.h>
#include <bromesh/primitives/primitives.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace bro;
using namespace bro::scene::vk;

namespace {

constexpr int kSize = 64;

struct Pixel {
    uint8_t r, g, b, a;
};

Pixel at(const std::vector<uint8_t>& px, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

/// A lit orange cube in front of the camera, a sun, nothing behind it.
scene::MeshNode* buildScene(scene::SceneGraph& graph) {
    graph.setCanvasSize(kSize, kSize);
    graph.setCamera(1.0f, 1.0f, 0.1f, 100.0f, {0.0f, 1.5f, 4.0f}, {0.0f, 0.0f, 0.0f});
    scene::MeshNode* cube = graph.createMesh("cube");
    cube->setMesh(bromesh::box(0.8f, 0.8f, 0.8f));
    cube->setColor(1.0f, 0.5f, 0.1f, 1.0f);
    graph.root()->addChild(cube);
    scene::LightNode* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection({-0.3f, -1.0f, -0.5f});
    sun->setIntensity(3.0f);
    graph.root()->addChild(sun);
    return cube;
}

std::vector<uint8_t> renderAndRead(scene::SceneGraph& graph) {
    graph.render();
    int w = 0, h = 0;
    std::vector<uint8_t> px = graph.readTonemapPixelsRGBA(w, h);
    assert(w == kSize && h == kSize && px.size() == static_cast<size_t>(kSize * kSize * 4));
    return px;
}

}  // namespace

int main() {
    std::cout << "=== bro_vulkan_scene_passes_test: scene passes and shaders ===" << std::endl;

    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;

    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::cerr << "Failed to initialize headless Vulkan context!" << std::endl;
        return 1;
    }

    SceneVkDevice device(context);
    if (!device.init()) {
        std::cerr << "Failed to initialize SceneVkDevice!" << std::endl;
        return 1;
    }

    // -------------------------------------------------------------------------
    // Test 1: SceneVkShaderCompiler & SPIR-V Pipeline
    // -------------------------------------------------------------------------
    std::cout << "[Test 1] SceneVkShaderCompiler (built-in SPIR-V & run-time GLSL)... " << std::flush;
    {
        // 1. Verify built-in shader bytecode
        const auto& meshVs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::MeshVert);
        const auto& meshFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::MeshFrag);
        const auto& shadowVs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::ShadowVert);
        const auto& envFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::SkyAtmosphereFrag);
        const auto& tonemapFs = SceneVkShaderCompiler::getBuiltinSpirv(BuiltinSceneShader::TonemapFrag);

        assert(!meshVs.empty() && meshVs[0] == 0x07230203);
        assert(!meshFs.empty() && meshFs[0] == 0x07230203);
        assert(!shadowVs.empty() && shadowVs[0] == 0x07230203);
        assert(!envFs.empty() && envFs[0] == 0x07230203);
        assert(!tonemapFs.empty() && tonemapFs[0] == 0x07230203);

        // 2. Runtime GLSL compilation (in-process glslang)
        {
            std::string testGlsl =
                "#version 450\n"
                "layout(location = 0) in vec3 pos;\n"
                "layout(location = 0) out vec3 outPos;\n"
                "void main() {\n"
                "    gl_Position = vec4(pos, 1.0);\n"
                "    outPos = pos;\n"
                "}\n";
            auto compiled = SceneVkShaderCompiler::compileGlsl(testGlsl, VK_SHADER_STAGE_VERTEX_BIT);
            assert(!compiled.empty());
            assert(compiled[0] == 0x07230203);

            // Test shader module creation and deletion
            VkShaderModule mod = SceneVkShaderCompiler::createModule(device.device(), compiled);
            assert(mod != VK_NULL_HANDLE);
            SceneVkShaderCompiler::destroyModule(device.device(), mod);
        }

        std::cout << "PASSED" << std::endl;
    }

    device.shutdown();
    scene::SceneRenderer::setDefaultVulkanContext(&context);

    // -------------------------------------------------------------------------
    // Test 2: the default pass list draws a lit cube over a transparent clear
    // -------------------------------------------------------------------------
    std::cout << "[Test 2] Scene renderer: default passes, lit cube... " << std::flush;
    {
        scene::SceneGraph graph;
        buildScene(graph);
        const std::vector<uint8_t> px = renderAndRead(graph);
        assert(graph.hasMeshContent());
        const Pixel centre = at(px, kSize / 2, kSize / 2);
        const Pixel corner = at(px, 1, 1);
        assert(centre.a == 255 && centre.r > centre.b);
        assert(corner.a == 0);
        const render::LayerImage out = graph.renderer().outputImage();
        assert(out && out.width == kSize && out.height == kSize);
        assert(out.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 3: every optional pass at once (MSAA, SSAO, SSR, DoF, bloom, FXAA,
    // sky, light icons), then a destroyed node's resources released
    // -------------------------------------------------------------------------
    std::cout << "[Test 3] Scene renderer: all optional passes, node release... " << std::flush;
    {
        scene::SceneGraph graph;
        scene::MeshNode* cube = buildScene(graph);
        graph.setMSAA(4);
        graph.renderer().setSSAO(true, 0.5f, 1.0f, 0.025f);
        graph.renderer().setSSR(true, 30.0f, 48, 0.3f, 1.0f, 0.1f);
        graph.renderer().setDepthOfField(true, 4.0f, 2.0f, 4.0f);
        graph.renderer().setBloom(true, 0.8f, 0.3f, 2.0f);
        graph.renderer().setFXAA(true);
        graph.renderer().setShowLightIcons(true);
        scene::AtmosphereParams atmosphere;
        atmosphere.enabled = true;
        graph.renderer().setAtmosphere(atmosphere);

        std::vector<uint8_t> px = renderAndRead(graph);
        assert(at(px, kSize / 2, kSize / 2).a == 255);
        px = renderAndRead(graph);   // second frame: inline readback path, loaded scopes
        assert(at(px, kSize / 2, kSize / 2).a == 255);

        graph.destroyNode(cube);
        px = renderAndRead(graph);
        assert(at(px, 1, 1).a == 255);   // the sky still covers the frame
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 4: a particle texture is sRGB: a mid-grey texel samples as its
    // linear value, the same light as a white texture tinted by that value.
    // -------------------------------------------------------------------------
    std::cout << "[Test 4] Scene renderer: particle textures decode sRGB... " << std::flush;
    {
        const auto dir = std::filesystem::temp_directory_path();
        const auto writeFlat = [&](const char* name, uint8_t v) {
            const std::vector<uint8_t> rgba(16 * 16 * 4, v);
            std::vector<uint8_t> px = rgba;
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            const std::string path = (dir / name).string();
            assert(broimage::encode_png_file(path, px.data(), 16, 16, 4));
            return path;
        };
        const std::string grey = writeFlat("bro_scene_particle_grey.png", 128);
        const std::string white = writeFlat("bro_scene_particle_white.png", 255);
        const float linearGrey = std::pow((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f);

        const auto centreOf = [&](const std::string& texture, float tint) {
            scene::SceneGraph graph;
            graph.setCanvasSize(kSize, kSize);
            graph.setCamera(1.0f, 1.0f, 0.1f, 100.0f, {0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f});
            graph.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f, 1.0f);
            scene::Particles3DNode* p = graph.createParticles3D("p");
            graph.root()->addChild(p);
            p->setTexturePath(texture);
            p->setSpeed(0.0f, 0.0f);
            p->setLifetime(10.0f, 10.0f);
            p->setSize(3.0f, 3.0f);
            p->setColors({tint, tint, tint, 1.0f}, {tint, tint, tint, 1.0f});
            p->burst(1);
            return at(renderAndRead(graph), kSize / 2, kSize / 2);
        };
        const Pixel g = centreOf(grey, 1.0f);
        const Pixel w = centreOf(white, linearGrey);
        assert(g.a == 255 && w.a == 255);
        assert(std::abs(int(g.r) - int(w.r)) <= 3);
        assert(g.r < 80);   // stored UNORM, the grey would come out near 128
        std::remove(grey.c_str());
        std::remove(white.c_str());
        std::cout << "PASSED" << std::endl;
    }

    scene::SceneRenderer::setDefaultVulkanContext(nullptr);
    if (const uint32_t errors = render::vulkanValidationErrorCount()) {
        std::cerr << "FAILED: " << errors << " Vulkan validation error(s)" << std::endl;
        return 1;
    }
    std::cout << "=== bro_vulkan_scene_passes_test: all checks passed ===" << std::endl;
    return 0;
}
