#include "scene/nodes/voxel_world_node.h"
#include "scene/nodes/light_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/sdf_gpu_tracer.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"

#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <brokernel/brokernel.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/voxel/voxel_dag.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace bro;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void auditStage(const char* label, const std::vector<uint32_t>& words, bool requireAllDivsProven) {
    check(words.size() > 16, std::string(label) + ": SPIR-V non-empty");
    if (words.empty()) return;
    check(words[0] == 0x07230203u, std::string(label) + ": valid SPIR-V magic");
    const auto report = brass::spirv::audit_compute_shader_binary(words);
    if (!report.ok()) {
        for (const auto& d : report.verifier_diagnostics) {
            std::fprintf(stderr, "audit[%s] verifier: %s\n", label, d.message.c_str());
        }
        for (const auto& h : report.hazards) {
            std::fprintf(stderr, "audit[%s] hazard: %s\n", label, h.message.c_str());
        }
    }
    check(report.parse_ok, std::string(label) + ": parses cleanly");
    check(report.structural_ok, std::string(label) + ": structural CFG ok");
    check(report.dominance_ok, std::string(label) + ": SSA dominance ok");
    check(report.barriers_uniform, std::string(label) + ": uniform barriers ok");
    check(report.shared_bounds_ok, std::string(label) + ": shared bounds ok");
    check(report.hazards.empty(), std::string(label) + ": zero hazards");
    check(report.div_sqrt_ok, std::string(label) + ": div/sqrt safety ok");
    check(report.ok(), std::string(label) + ": brass::verify audit.ok()");
    if (requireAllDivsProven) {
        if (!report.all_div_sqrt_proven) {
            for (const auto& dv : report.divisions) {
                if (!dv.proven_safe) {
                    std::fprintf(stderr, "audit[%s] unproven div/sqrt in %s: %s\n",
                                 label, dv.function.c_str(), dv.guard_reason.c_str());
                }
            }
        }
        check(report.all_div_sqrt_proven, std::string(label) + ": every div/sqrt proven safe");
    }
}

void testSpirvVerification() {
    // 1. Standalone pure `voxeldag` compute kernel auditing is_carved_at + trace_region + trace_region_caster.
    {
        brokernel::Compiler compiler;
        compiler.register_module("voxeldag", scene::vk::voxelDagModuleSource());
        const std::string kernelSrc = R"(
            import voxeldag;

            @kernel @grid(1)
            fn audit_voxeldag_kernel(
                g: grid<1>,
                regions: read_buffer<vec4<f32>>,
                nodes: read_buffer<vec4<u32>>,
                children: read_buffer<u32>,
                bricks: read_buffer<vec4<u32>>,
                attrs: read_buffer<u32>,
                clips: read_buffer<vec4<u32>>,
                frames: read_buffer<u32>,
                out_hits: buffer<vec4<f32>>
            ) {
                let id = g.x();
                let ro = vec3<f32>(0.0, 2.0, 8.0);
                let dir = vec3<f32>((id as f32) * 0.01 - 0.5, -0.2, -1.0);
                let rd = dir * (1.0 / max(sqrt(max(dot(dir, dir), 1e-8)), 1e-6));
                let hit = voxeldag::trace_region(
                    ro, rd, 200.0, 0.004, 1.0, 64u, 0u, 0u,
                    regions, nodes, children, bricks, attrs, clips, frames
                );
                var sh: f32 = 1.0;
                if (hit.hit) {
                    let sdir = vec3<f32>(-0.3, -1.0, -0.2);
                    let sun_rd = sdir * (1.0 / max(sqrt(max(dot(sdir, sdir), 1e-8)), 1e-6));
                    let cast = voxeldag::trace_region_caster(
                        hit.pos - sun_rd * 64.0, sun_rd, 128.0, 0.05, 64u, 0u, 0u, id as u32,
                        regions, nodes, children, bricks, attrs, clips, frames
                    );
                    sh = cast.hit ? 0.5 : 1.0;
                }
                let carved = voxeldag::is_carved_at(
                    ro + rd * 2.0, 1u, 0u, regions, nodes, children, bricks, clips, frames
                );
                let carved_static = voxeldag::is_carved_static_at(
                    ro + rd * 2.0, 1u, regions, nodes, children, bricks
                );
                out_hits[id] = vec4<f32>(hit.color * sh, (carved || carved_static) ? 1.0 : hit.t);
            }
        )";
        brokernel::CompileOptions opts;
        opts.entry_name = "audit_voxeldag_kernel";
        opts.compile_cpu = false;
        const auto res = compiler.compile_source(kernelSrc, "audit_voxeldag.bk", opts);
        const brokernel::CompiledKernel* k = res.ok ? res.find_kernel("audit_voxeldag_kernel") : nullptr;
        check(res.ok && k && !k->spirv.words.empty(),
              "compile standalone voxeldag audit kernel" + (res.ok ? std::string() : ": " + res.error_message()));
        if (k && !k->spirv.words.empty()) {
            auditStage("voxeldag_standalone_kernel", k->spirv.words, true);
        }
    }

    for (const char* entry : {"voxeldag_vert", "voxeldag_frag", "voxeldag_indirect_frag"}) {
        scene::vk::KernelHookStage stage;
        std::string err;
        const bool compiled = scene::vk::compileKernelVoxelDagStage(entry, stage, err);
        check(compiled, std::string("compile ") + entry + (compiled ? "" : ": " + err));
        if (compiled) auditStage(entry, stage.spirv, false);
    }

    const std::string carveHook = R"(
        module hook;
        import scene;
        import mesh;
        import voxeldag;

        struct Params { region_count: f32, anim_tick: f32, pad0: f32, pad1: f32 }
        @binding(4, 0) var params: uniform<Params>;
        @binding(4, 14) var regions: read_buffer<vec4<f32>>;
        @binding(4, 15) var nodes: read_buffer<vec4<u32>>;
        @binding(4, 16) var children: read_buffer<u32>;
        @binding(4, 17) var bricks: read_buffer<vec4<u32>>;
        @binding(4, 18) var anim_clips: read_buffer<vec4<u32>>;
        @binding(4, 19) var anim_frames: read_buffer<u32>;

        pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
            let wp = v.world_pos + scene::camera.eye_world.xyz;
            if (voxeldag::is_carved_at(wp, params.region_count as u32, params.anim_tick as u32,
                                         regions, nodes, children, bricks, anim_clips, anim_frames)) {
                discard_fragment();
            }
            return s;
        }
    )";

    scene::vk::KernelMeshHook meshHook;
    std::string hookErr;
    const bool hookOk = scene::vk::compileKernelMeshHook(
        scene::SceneRenderer::CustomShaderTarget::Static, carveHook, "carve_test.bk", {}, meshHook, hookErr);
    check(hookOk, "compile custom carve fragment hook" + (hookOk ? "" : ": " + hookErr));
    if (hookOk) auditStage("custom_carve_frag", meshHook.fs.spirv, false);
}

void pixelAt(const std::vector<uint8_t>& rgba, int w, int h, int x, int y, uint8_t out[4]) {
    const int cx = std::clamp(x, 0, w - 1);
    const int cy = std::clamp(y, 0, h - 1);
    const size_t idx = (static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)) * 4u;
    for (int k = 0; k < 4; ++k) out[k] = rgba[idx + k];
}

void testDensePointerForestRender(render::VulkanContext& context) {
    const uint32_t err0 = render::vulkanValidationErrorCount();
    scene::SceneGraph graph;
    graph.setCanvasSize(256, 256);
    graph.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f);
    graph.setAmbient(0.4f, 0.4f, 0.4f);
    graph.setCamera(0.95f, 1.0f, 0.5f, 600.0f, {0.0f, 38.0f, 95.0f}, {0.0f, 0.0f, 0.0f});

    auto* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection(bromath::vnorm(bromath::Vec3{-0.4f, -1.0f, -0.3f}));
    sun->setIntensity(2.5f);
    sun->setCastsShadow(false);
    graph.root()->addChild(sun);

    auto* world = graph.createVoxelWorld("forest");
    graph.root()->addChild(world);

    bromesh::DagVoxelizeOptions opts;
    opts.resolution = 16;
    opts.defaultColor[0] = 0.12f;
    opts.defaultColor[1] = 0.85f;
    opts.defaultColor[2] = 0.22f;
    opts.defaultColor[3] = 1.0f;
    opts.roughness = 0.8f;

    int archDepth = 0;
    const uint32_t treeArch = bromesh::voxelizeSdfToDag(
        world->pool(),
        [](float x, float y, float z) {
            const float crown = std::sqrt(x * x + (y - 0.15f) * (y - 0.15f) + z * z) - 0.72f;
            const float trunk = std::max(std::sqrt(x * x + z * z) - 0.2f, std::abs(y + 0.45f) - 0.45f);
            return std::min(crown, trunk);
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        opts,
        &archDepth);
    check(treeArch != bromesh::kDagNullNode && archDepth == 2, "SDF tree archetype built");

    const size_t baseBricks = world->pool().bricks().size();
    const size_t baseAttrs = world->pool().attrs().size();

    scene::VoxelRegion reg;
    reg.voxelSize = 0.25f;
    reg.maxDepth = 8;  // extent = 0.25 * (4 << 8) = 256 -> [-128, +128]^3
    reg.flags = scene::kVoxelRegionEnabled;
    const uint32_t rid = world->addRegion(reg);

    std::vector<bromesh::DagPlacement> batch;
    batch.reserve(64 * 64);
    for (int iz = 0; iz < 64; ++iz) {
        for (int ix = 0; ix < 64; ++ix) {
            batch.push_back({ix, 31, iz, treeArch, false});
        }
    }
    check(world->placeSubtreesBatch(rid, batch, 6), "placed 4,096 tree instances via batch");
    check(world->pool().bricks().size() == baseBricks, "zero duplicate bricks across 4,096 trees");
    check(world->pool().attrs().size() == baseAttrs, "zero duplicate voxel attrs across 4,096 trees");
    check(world->pool().nodes().size() < 600, "content-addressed nodes < 600 for 4,096 trees");

    graph.renderForRead();
    int w = 0, h = 0;
    const auto px = graph.readTonemapPixelsRGBA(w, h);
    check(w == 256 && h == 256 && px.size() == 256u * 256u * 4u, "forest readback 256x256");

    int greenPixels = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 1] > px[i] + 25 && px[i + 1] > px[i + 2] + 25 && px[i + 1] > 45) {
            ++greenPixels;
        }
    }
    check(greenPixels > 4000, "dense 4096-tree canopy clearly visible (" + std::to_string(greenPixels) + " px)");
    check(render::vulkanValidationErrorCount() == err0, "forest render zero Vulkan validation errors");
    (void)context;
}

void testZeroCopyAnimationTick(render::VulkanContext& context) {
    const uint32_t err0 = render::vulkanValidationErrorCount();
    scene::SceneGraph graph;
    graph.setCanvasSize(128, 128);
    graph.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f);
    graph.setAmbient(0.55f, 0.55f, 0.55f);
    graph.setCamera(0.8f, 1.0f, 0.5f, 100.0f, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f});

    auto* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection(bromath::vnorm(bromath::Vec3{0.0f, -0.4f, -1.0f}));
    sun->setIntensity(2.2f);
    sun->setCastsShadow(false);
    graph.root()->addChild(sun);

    auto* world = graph.createVoxelWorld("anim");
    world->setAutoAnimate(false);
    graph.root()->addChild(world);

    auto makeBox = [&](float r, float g, float b) -> uint32_t {
        bromesh::DagVoxelizeOptions o;
        o.resolution = 8;
        o.defaultColor[0] = r;
        o.defaultColor[1] = g;
        o.defaultColor[2] = b;
        o.defaultColor[3] = 1.0f;
        o.fillInterior = true;
        return bromesh::voxelizeSdfToDag(
            world->pool(),
            [](float x, float y, float z) {
                return std::max(std::abs(x), std::max(std::abs(y), std::abs(z))) - 0.9f;
            },
            bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
            o);
    };

    const uint32_t redRoot = makeBox(0.95f, 0.08f, 0.08f);
    const uint32_t blueRoot = makeBox(0.08f, 0.12f, 0.95f);
    const uint32_t clipIdx = world->pool().registerAnimClip({redRoot, blueRoot});

    scene::VoxelRegion reg;
    reg.voxelSize = 0.25f;
    reg.maxDepth = 2;  // extent = 0.25 * 16 = 4 -> [-2, +2]^3 centered at 0
    reg.flags = scene::kVoxelRegionEnabled;
    const uint32_t rid = world->addRegion(reg);
    for (int oz = 0; oz < 2; ++oz) {
        for (int oy = 0; oy < 2; ++oy) {
            for (int ox = 0; ox < 2; ++ox) {
                world->placeAnimatedSubtree(rid, ox, oy, oz, clipIdx, 0u, false, 1u);
            }
        }
    }

    const uint64_t genBefore = world->pool().generation();
    world->setAnimTick(0u);
    graph.renderForRead();
    int w = 0, h = 0;
    const auto f0 = graph.readTonemapPixelsRGBA(w, h);
    uint8_t c0[4] = {};
    pixelAt(f0, w, h, w / 2, h / 2, c0);
    check(c0[0] > c0[2] + 60 && c0[0] > 100, "anim tick 0 shows red frame");

    world->setAnimTick(1u);
    check(world->pool().generation() == genBefore, "anim tick change does zero DAG pool rebuild");
    graph.renderForRead();
    const auto f1 = graph.readTonemapPixelsRGBA(w, h);
    uint8_t c1[4] = {};
    pixelAt(f1, w, h, w / 2, h / 2, c1);
    check(c1[2] > c1[0] + 60 && c1[2] > 100, "anim tick 1 flips to blue frame cleanly");
    check(render::vulkanValidationErrorCount() == err0, "anim render zero Vulkan validation errors");
    (void)context;
}

bromesh::MeshData makeGroundPlane(float halfExtent, int segs) {
    bromesh::MeshData m;
    const int n = segs + 1;
    m.positions.reserve(static_cast<size_t>(n) * n * 3u);
    m.normals.reserve(static_cast<size_t>(n) * n * 3u);
    m.uvs.reserve(static_cast<size_t>(n) * n * 2u);
    for (int z = 0; z < n; ++z) {
        const float vz = -halfExtent + 2.0f * halfExtent * static_cast<float>(z) / static_cast<float>(segs);
        for (int x = 0; x < n; ++x) {
            const float vx = -halfExtent + 2.0f * halfExtent * static_cast<float>(x) / static_cast<float>(segs);
            m.positions.push_back(vx);
            m.positions.push_back(0.0f);
            m.positions.push_back(vz);
            m.normals.push_back(0.0f);
            m.normals.push_back(1.0f);
            m.normals.push_back(0.0f);
            m.uvs.push_back(static_cast<float>(x) / static_cast<float>(segs));
            m.uvs.push_back(static_cast<float>(z) / static_cast<float>(segs));
        }
    }
    for (int z = 0; z < segs; ++z) {
        for (int x = 0; x < segs; ++x) {
            const uint32_t a = static_cast<uint32_t>(z * n + x);
            const uint32_t b = a + 1u;
            const uint32_t c = a + static_cast<uint32_t>(n);
            const uint32_t d = c + 1u;
            m.indices.push_back(a);
            m.indices.push_back(c);
            m.indices.push_back(b);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(d);
        }
    }
    return m;
}

void testCarvedCaveAndDepthInterop(render::VulkanContext& context) {
    const uint32_t err0 = render::vulkanValidationErrorCount();
    scene::SceneGraph graph;
    graph.setCanvasSize(256, 256);
    graph.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f);
    graph.setAmbient(0.45f, 0.45f, 0.45f);
    graph.setCamera(0.75f, 1.0f, 0.2f, 200.0f, {0.0f, 4.8f, 6.5f}, {0.0f, -0.5f, 0.0f});

    auto* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection(bromath::vnorm(bromath::Vec3{-0.2f, -1.0f, -0.4f}));
    sun->setIntensity(2.4f);
    sun->setCastsShadow(false);
    graph.root()->addChild(sun);

    auto* world = graph.createVoxelWorld("cave_world");
    graph.root()->addChild(world);

    // Region covers [-4, +4]^3 around (0, 0, 0) using 16^3 voxels at depth=2 (0.5m voxels).
    // Carve a vertical cave shaft in x,z in [-1.6, 1.6], y in [-1.6, 1.6] and fill an orange cave floor at y < -1.0.
    bromesh::DagVoxelizeOptions carveOpts;
    carveOpts.resolution = 16;
    carveOpts.state = bromesh::VoxelState::Carve;
    const uint32_t carveRoot = bromesh::voxelizeSdfToDag(
        world->pool(),
        [](float x, float y, float z) {
            return std::max(std::abs(x) - 0.40f, std::max(std::abs(y) - 0.45f, std::abs(z) - 0.40f));
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        carveOpts);

    bromesh::DagVoxelizeOptions solidOpts;
    solidOpts.resolution = 16;
    solidOpts.state = bromesh::VoxelState::Solid;
    solidOpts.defaultColor[0] = 0.96f;
    solidOpts.defaultColor[1] = 0.42f;
    solidOpts.defaultColor[2] = 0.08f;
    solidOpts.defaultColor[3] = 1.0f;
    solidOpts.fillInterior = true;
    const uint32_t solidRoot = bromesh::voxelizeSdfToDag(
        world->pool(),
        [](float x, float y, float z) {
            // Cave floor below y=-0.32 plus a rocky right overhang arch at x in [0.15, 0.55], y in [0.05, 0.45]
            const float floorSdf = std::max(std::abs(y + 0.52f) - 0.22f, std::max(std::abs(x) - 0.7f, std::abs(z) - 0.7f));
            const float archSdf = std::max(std::abs(x - 0.30f) - 0.22f, std::max(std::abs(y - 0.15f) - 0.20f, std::abs(z) - 0.35f));
            return std::min(floorSdf, archSdf);
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        solidOpts);

    const uint32_t combinedRoot = world->pool().mergeSubtrees(carveRoot, false, solidRoot, false);
    scene::VoxelRegion reg;
    reg.voxelSize = 0.5f;
    reg.maxDepth = 2;  // extent = 0.5 * 16 = 8 -> [-4, +4]^3
    reg.flags = scene::kVoxelRegionEnabled;
    const uint32_t rid = world->addRegion(reg);
    check(world->setRegionRoot(rid, combinedRoot), "set combined carve+solid root on region");
    check((world->findRegion(rid)->flags & scene::kVoxelRegionHasCarve) != 0u,
          "region automatically tagged kVoxelRegionHasCarve");

    check(world->syncBuffers(context.compute()), "sync shared voxel GPU buffers");
    const auto& bufs = world->sharedBuffers();

    // Flat green ground plane with custom voxeldag::is_carved_at fragment discard hook
    auto* ground = graph.createMesh("ground");
    ground->setMesh(makeGroundPlane(12.0f, 48));
    ground->setColor(0.12f, 0.75f, 0.18f, 1.0f);
    ground->setRoughness(0.85f);
    ground->setCastsShadow(false);
    ground->setCustomShaderModule(R"(
        module hook;
        import scene;
        import mesh;
        import voxeldag;

        struct Params { region_count: f32, anim_tick: f32, pad0: f32, pad1: f32 }
        @binding(4, 0) var params: uniform<Params>;
        @binding(4, 14) var regions: read_buffer<vec4<f32>>;
        @binding(4, 15) var nodes: read_buffer<vec4<u32>>;
        @binding(4, 16) var children: read_buffer<u32>;
        @binding(4, 17) var bricks: read_buffer<vec4<u32>>;
        @binding(4, 18) var anim_clips: read_buffer<vec4<u32>>;
        @binding(4, 19) var anim_frames: read_buffer<u32>;

        pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
            let wp = v.world_pos + scene::camera.eye_world.xyz;
            if (voxeldag::is_carved_at(wp, params.region_count as u32, params.anim_tick as u32,
                                         regions, nodes, children, bricks, anim_clips, anim_frames)) {
                discard_fragment();
            }
            return s;
        }
    )", "terrain_carve.bk");
    const float rc = static_cast<float>(bufs.regionCount);
    const float tickF = 0.0f;
    ground->setCustomShaderUniform("region_count", 1, &rc);
    ground->setCustomShaderUniform("anim_tick", 1, &tickF);
    ground->setCustomShaderBuffer("regions", bufs.regions);
    ground->setCustomShaderBuffer("nodes", bufs.nodes);
    ground->setCustomShaderBuffer("children", bufs.children);
    ground->setCustomShaderBuffer("bricks", bufs.bricks);
    ground->setCustomShaderBuffer("anim_clips", bufs.animClips);
    ground->setCustomShaderBuffer("anim_frames", bufs.animFrames);
    graph.root()->addChild(ground);

    // Foreground opaque blue raster cube on the left half overlapping part of the voxel region
    auto* fgCube = graph.createMesh("fg_cube");
    fgCube->setMesh(bromesh::box(0.9f, 0.9f, 0.9f));
    fgCube->setPosition(-2.1f, 1.4f, 2.2f);
    fgCube->setColor(0.10f, 0.25f, 0.95f, 1.0f);
    fgCube->setRoughness(0.5f);
    fgCube->setCastsShadow(false);
    graph.root()->addChild(fgCube);

    graph.renderForRead();
    int w = 0, h = 0;
    const auto px = graph.readTonemapPixelsRGBA(w, h);
    check(w == 256 && h == 256, "cave render 256x256");

    int greenTerrainPx = 0;
    int orangeCavePx = 0;
    int blueCubePx = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t c[4] = {};
            pixelAt(px, w, h, x, y, c);
            if (c[1] > c[0] + 25 && c[1] > c[2] + 25 && c[1] > 45) ++greenTerrainPx;
            if (c[0] > c[1] + 25 && c[0] > c[2] + 50 && c[0] > 90) ++orangeCavePx;
            if (c[2] > c[0] + 50 && c[2] > c[1] + 25 && c[2] > 90) ++blueCubePx;
        }
    }
    check(greenTerrainPx > 1500, "uncarved green terrain intact (" + std::to_string(greenTerrainPx) + " px)");
    check(orangeCavePx > 300, "carved ground reveals 3D orange voxel cave/overhang (" + std::to_string(orangeCavePx) + " px)");
    check(blueCubePx > 800, "foreground raster blue cube stays unoccluded (" + std::to_string(blueCubePx) + " px)");

    // Center pixel hits the carved cave mouth directly -> must be orange voxel interior, NOT green terrain!
    uint8_t center[4] = {};
    pixelAt(px, w, h, w / 2, h / 2 + 8, center);
    check(center[0] > center[1] && center[0] > center[2] + 40,
          "center cave opening discards green heightfield and shows orange voxel interior");
    check(render::vulkanValidationErrorCount() == err0, "cave render zero Vulkan validation errors");
}

void testPlanetOriginAndStaticCarveAndDistanceBands(render::VulkanContext& context) {
    const uint32_t err0 = render::vulkanValidationErrorCount();
    scene::SceneGraph graph;
    graph.setCanvasSize(192, 192);
    graph.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f);
    graph.setAmbient(0.5f, 0.5f, 0.5f);
    graph.setCamera(0.75f, 1.0f, 0.5f, 400.0f, {0.0f, 3.0f, 0.0f}, {0.0f, -0.5f, -14.0f});
    graph.setCameraOrigin({600000.0, 0.0, 0.0});

    auto* sun = graph.createLight("sun");
    sun->setKind(scene::LightNode::Kind::Directional);
    sun->setDirection(bromath::vnorm(bromath::Vec3{-0.2f, -1.0f, -0.3f}));
    sun->setIntensity(2.4f);
    sun->setCastsShadow(false);
    graph.root()->addChild(sun);

    auto* world = graph.createVoxelWorld("planet_vox");
    graph.root()->addChild(world);

    bromesh::DagVoxelizeOptions cOpts;
    cOpts.resolution = 16;
    cOpts.state = bromesh::VoxelState::Carve;
    const uint32_t carveRoot = bromesh::voxelizeSdfToDag(
        world->pool(),
        [](float x, float y, float z) {
            return std::max(std::abs(x) - 0.5f, std::max(std::abs(y) - 0.5f, std::abs(z) - 0.5f));
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        cOpts);

    bromesh::DagVoxelizeOptions sOpts;
    sOpts.resolution = 16;
    sOpts.state = bromesh::VoxelState::Solid;
    sOpts.defaultColor[0] = 0.96f;
    sOpts.defaultColor[1] = 0.42f;
    sOpts.defaultColor[2] = 0.08f;
    sOpts.emissive = 0.15f;
    sOpts.fillInterior = true;
    const uint32_t solidRoot = bromesh::voxelizeSdfToDag(
        world->pool(),
        [](float x, float y, float z) {
            return std::max(std::abs(y + 0.45f) - 0.25f, std::max(std::abs(x) - 0.7f, std::abs(z) - 0.7f));
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        sOpts);

    // Region centered at planet-scale coordinate [600000.0, 0.0, -14.0], extent = 8m ([-4, 4]^3)
    scene::VoxelRegion reg;
    reg.center[0] = 600000.0;
    reg.center[1] = 0.0;
    reg.center[2] = -14.0;
    reg.voxelSize = 0.5f;
    reg.maxDepth = 2;
    reg.minDist = 4.0f;
    reg.fadeStart = 35.0f;
    reg.maxDist = 60.0f;
    reg.flags = scene::kVoxelRegionEnabled;
    const uint32_t rid = world->addRegion(reg);
    world->setRegionRoot(rid, world->pool().mergeSubtrees(carveRoot, false, solidRoot, false));

    // Culled far region with minDist > 120m must stay invisible
    scene::VoxelRegion hiddenReg;
    hiddenReg.center[0] = 600000.0;
    hiddenReg.center[1] = 0.0;
    hiddenReg.center[2] = -14.0;
    hiddenReg.voxelSize = 0.5f;
    hiddenReg.maxDepth = 2;
    hiddenReg.minDist = 120.0f;
    hiddenReg.flags = scene::kVoxelRegionEnabled;
    const uint32_t hid = world->addRegion(hiddenReg);
    world->setRegionRoot(hid, solidRoot);
    // Keep only main region in shared buffer test by creating ground hook relative to [600000, 0, 0]
    world->removeRegion(hid);

    check(world->syncBuffers(context.compute()), "planet static carve buffer sync");
    const auto& bufs = world->sharedBuffers();

    auto* ground = graph.createMesh("planet_ground");
    ground->setMesh(makeGroundPlane(30.0f, 24));
    ground->setPosition(0.0f, 0.0f, -14.0f);
    ground->setColor(0.10f, 0.72f, 0.22f, 1.0f);
    ground->setCastsShadow(false);
    ground->setCustomShaderModule(R"(
        module hook;
        import scene;
        import mesh;
        import voxeldag;

        struct Params { carve_rel: vec3<f32>, region_count: f32 }
        @binding(4, 0) var params: uniform<Params>;
        @binding(4, 17) var regions: read_buffer<vec4<f32>>;
        @binding(4, 20) var nodes: read_buffer<vec4<u32>>;
        @binding(4, 21) var children: read_buffer<u32>;
        @binding(4, 22) var bricks: read_buffer<vec4<u32>>;

        pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
            if (voxeldag::is_carved_static_at(params.carve_rel + v.world_pos, params.region_count as u32,
                                                regions, nodes, children, bricks)) {
                discard_fragment();
            }
            return s;
        }
    )", "planet_carve_static.bk");
    const float carveRel[3] = {600000.0f, 3.0f, 0.0f};
    const float rc = static_cast<float>(bufs.regionCount);
    ground->setCustomShaderUniform("carve_rel", 3, carveRel);
    ground->setCustomShaderUniform("region_count", 1, &rc);
    ground->setCustomShaderBuffer("regions", bufs.regions);
    ground->setCustomShaderBuffer("nodes", bufs.nodes);
    ground->setCustomShaderBuffer("children", bufs.children);
    ground->setCustomShaderBuffer("bricks", bufs.bricks);
    graph.root()->addChild(ground);

    graph.renderForRead();
    int w = 0, h = 0;
    const auto px = graph.readTonemapPixelsRGBA(w, h);
    int orangePx = 0, greenPx = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 1] > px[i] + 20 && px[i + 1] > px[i + 2] + 20 && px[i + 1] > 40) ++greenPx;
        if (px[i] > px[i + 1] + 20 && px[i] > px[i + 2] + 40 && px[i] > 80) ++orangePx;
    }
    check(greenPx > 500, "planet-origin ground rendered (" + std::to_string(greenPx) + " px)");
    uint8_t midPx[4] = {};
    pixelAt(px, w, h, w / 2, h / 2, midPx);
    if (orangePx <= 120) {
        std::fprintf(stderr, "DEBUG planet mid=(%u,%u,%u,%u) green=%d orange=%d\n",
                     midPx[0], midPx[1], midPx[2], midPx[3], greenPx, orangePx);
    }
    check(orangePx > 120, "planet-origin 4-buffer static carve reveals voxel cave (" + std::to_string(orangePx) + " px)");

    // Now set minDist = 120m on rid -> region must be culled cleanly
    scene::VoxelRegion culled = *world->findRegion(rid);
    culled.minDist = 120.0f;
    culled.fadeStart = 0.0f;
    culled.maxDist = 0.0f;
    world->updateRegion(rid, culled);
    graph.renderForRead();
    const auto pxCulled = graph.readTonemapPixelsRGBA(w, h);
    int orangeCulled = 0;
    for (size_t i = 0; i < pxCulled.size(); i += 4) {
        if (pxCulled[i] > pxCulled[i + 1] + 20 && pxCulled[i] > pxCulled[i + 2] + 40 && pxCulled[i] > 80) ++orangeCulled;
    }
    check(orangeCulled == 0, "minDist = 120m culls closer region (" + std::to_string(orangeCulled) + " orange px)");
    check(render::vulkanValidationErrorCount() == err0, "planet origin + static carve zero Vulkan validation errors");
}

}  // namespace

int main() {
    testSpirvVerification();

    render::VulkanContextConfig cfg;
    cfg.enableValidation = true;
    cfg.headless = true;
    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::printf("SKIP: no Vulkan device for headless SVDAG tests\n");
        return g_failures == 0 ? 0 : 1;
    }

    scene::SceneRenderer::setDefaultVulkanContext(&context);
    testDensePointerForestRender(context);
    testZeroCopyAnimationTick(context);
    testCarvedCaveAndDepthInterop(context);
    testPlanetOriginAndStaticCarveAndDistanceBands(context);
    scene::SceneRenderer::setDefaultVulkanContext(nullptr);

    std::printf("bro_voxel_dag_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
