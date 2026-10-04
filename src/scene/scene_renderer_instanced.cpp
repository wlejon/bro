#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "mesh.frag.h"
#include "mesh_instanced.vert.h"
#include "foliage_scatter.vert.h"
#include "branch_tube.vert.h"
#include "shadow.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// Build the instanced fragment shader by mutating the regular kMeshFragSrc:
// add `in vec4 vInstColor;`, an optional atlas-grid UV remap on the base
// color sample, and multiply baseColor by the instance RGB tint. Done at
// runtime so the two shaders cannot drift apart accidentally. Declared in
// scene_renderer_internal.h — the custom-shader path (ensureCustomProgram,
// scene_renderer_mesh.cpp) splices user fragment chunks into this source.
std::string makeMeshInstancedFragSrc() {
    std::string s = withAtmosphere(kMeshFragSrc);
    // A miss on any anchor below means mesh.frag was edited without updating
    // this derivation — the injection would silently be skipped and instanced
    // tint/atlas would silently break, so make it loud (and fatal in Debug).
    auto anchorMissing = [](const std::string& anchor) {
        LOG_ERROR("makeMeshInstancedFragSrc: anchor \"%s\" not found in "
                  "mesh.frag — instanced injection skipped (mesh.frag edited "
                  "without updating this derivation?)", anchor.c_str());
        assert(!"makeMeshInstancedFragSrc: anchor missing in mesh.frag");
    };
    // Add the instance-only varying + uniform alongside the existing
    // varyings. uAlphaCutoff is already declared (and applied) by the base
    // kMeshFragSrc, so re-declaring it here would be a GLSL redeclaration
    // error — inject only what's unique to the instanced path.
    const std::string anchor1 = "in vec3 vBitangentW;";
    auto p = s.find(anchor1);
    if (p != std::string::npos) {
        s.insert(p + anchor1.size(),
                 "\nin vec4 vInstColor;\nuniform vec2 uAtlasGrid;");
    } else {
        anchorMissing(anchor1);
    }
    // Replace the baseColor texture sample so it can pick a sub-rect of the
    // texture when uAtlasGrid > 1. Only the baseColor sampler uses atlas UV;
    // normal/MR/AO/emissive textures keep the raw vUV (leaf cards usually
    // have a baseColor only). The cell index is read from vInstColor.a as
    // packed by setInstancesFromPosQuatScale: cell = int(a * 256).
    const std::string anchor2 = "vec4 tex = texture(uBaseColorTex, vUV);";
    p = s.find(anchor2);
    if (p != std::string::npos) {
        s.replace(p, anchor2.size(),
                  "vec2 uvForBase = vUV;\n"
                  "        if (uAtlasGrid.x > 1.0 || uAtlasGrid.y > 1.0) {\n"
                  "            int cell = int(vInstColor.a * 256.0);\n"
                  "            int cols = int(uAtlasGrid.x); if (cols < 1) cols = 1;\n"
                  "            int rows = int(uAtlasGrid.y); if (rows < 1) rows = 1;\n"
                  "            int total = cols * rows;\n"
                  "            if (cell < 0) cell = 0;\n"
                  "            if (cell >= total) cell = total - 1;\n"
                  "            int cx = cell - (cell / cols) * cols;\n"
                  "            int cy = cell / cols;\n"
                  "            vec2 cellSize = vec2(1.0 / float(cols), 1.0 / float(rows));\n"
                  "            uvForBase = (vec2(float(cx), float(cy)) + fract(vUV)) * cellSize;\n"
                  "        }\n"
                  "        vec4 tex = texture(uBaseColorTex, uvForBase);");
    } else {
        anchorMissing(anchor2);
    }
    // Multiply the resolved baseColor by the instance RGB tint right after
    // the base-color/alpha resolution block. Alpha is reserved for the atlas
    // index — never multiplied into baseAlpha. The alpha-cutoff discard is
    // inherited from the base shader, so it is not re-injected here.
    const std::string anchor3 = "        baseAlpha = uColor.a;\n    }\n";
    p = s.find(anchor3);
    if (p != std::string::npos) {
        s.insert(p + anchor3.size(),
                 "    baseColor *= vInstColor.rgb;\n");
    } else {
        anchorMissing(anchor3);
    }
    return s;
}

void SceneRenderer::queryInstancedUniformLocs(GLuint /*prog*/, InstancedDrawLocs& /*d*/,
                                              MeshProgramLocs& /*l*/) {
}

void SceneRenderer::ensureInstancedMeshPipeline() {
}

void SceneRenderer::ensureFoliageScatterPipeline() {
}

void SceneRenderer::ensureTubePipeline() {
}

void SceneRenderer::ensureTubeDepthPipeline() {
}

void SceneRenderer::renderInstancedMeshNode(InstancedMeshNode* /*mesh*/,
                                            const InstancedDrawLocs& /*L*/) {
}

}  // namespace bro::scene
