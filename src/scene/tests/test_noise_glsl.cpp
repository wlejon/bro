// FastNoise2 trees as GLSL (brokit's noise_glsl.h) against FastNoise2: each
// tree runs on the CPU (GenSingle2D / 3D) and, translated, in a
// compute shader at the same positions, and the two must agree to float
// rounding. FastNoise2 runs relaxed SIMD (fused multiply-adds, approximate
// reciprocals), so a point may land on the other side of a cell's edge from
// the GPU's; such points are counted, and must stay rare.

#include "render/glsl_compiler.h"
#include "render/vulkan_context.h"
#include "render/vulkan_util.h"

#include "api/noise_glsl.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace bro;
using FastNoise::Metadata;
using FastNoise::NodeData;

namespace {

int failures = 0;

// A tree built as FastNoise2's own data, its members set by name.
struct Tree {
    std::vector<std::unique_ptr<NodeData>> nodes;

    NodeData* node(const char* type) {
        for (const Metadata* m : Metadata::GetAll())
            if (std::strcmp(m->name, type) == 0) {
                nodes.push_back(std::make_unique<NodeData>(m));
                return nodes.back().get();
            }
        std::fprintf(stderr, "no FastNoise node %s\n", type);
        std::abort();
    }
    static void var(NodeData* n, const char* name, float f, int dim = -1) {
        const auto& vars = n->metadata->memberVariables;
        for (size_t i = 0; i < vars.size(); ++i)
            if (std::strcmp(vars[i].name, name) == 0 && vars[i].dimensionIdx == dim) {
                if (vars[i].type == Metadata::MemberVariable::EFloat) n->variables[i].f = f;
                else n->variables[i].i = static_cast<int>(f);
                return;
            }
        std::fprintf(stderr, "%s has no variable %s\n", n->metadata->name, name);
        std::abort();
    }
    static void src(NodeData* n, const char* name, NodeData* s) {
        const auto& members = n->metadata->memberNodeLookups;
        for (size_t i = 0; i < members.size(); ++i)
            if (std::strcmp(members[i].name, name) == 0) {
                n->nodeLookups[i] = s;
                return;
            }
        std::fprintf(stderr, "%s has no source %s\n", n->metadata->name, name);
        std::abort();
    }
    static void hyb(NodeData* n, const char* name, NodeData* s, float f = 0.0f, int dim = -1) {
        const auto& members = n->metadata->memberHybrids;
        for (size_t i = 0; i < members.size(); ++i)
            if (std::strcmp(members[i].name, name) == 0 && members[i].dimensionIdx == dim) {
                n->hybrids[i] = {s, f};
                return;
            }
        std::fprintf(stderr, "%s has no hybrid %s\n", n->metadata->name, name);
        std::abort();
    }
};

struct Gpu {
    render::VulkanContext& context;
    VkPhysicalDeviceMemoryProperties memory{};

    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    explicit Gpu(render::VulkanContext& c) : context(c) {
        vkGetPhysicalDeviceMemoryProperties(c.physicalDevice(), &memory);
    }

    Buffer buffer(size_t bytes) {
        Buffer b;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(context.device(), &bi, nullptr, &b.buffer);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(context.device(), b.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = *render::findMemoryType(
            memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(context.device(), &ai, nullptr, &b.memory);
        vkBindBufferMemory(context.device(), b.buffer, b.memory, 0);
        vkMapMemory(context.device(), b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        return b;
    }
    void destroy(Buffer& b) {
        vkDestroyBuffer(context.device(), b.buffer, nullptr);
        vkFreeMemory(context.device(), b.memory, nullptr);
    }

    /// `field` (a function float field(vecD p, int seed)) at `points` (xyz
    /// a vec4), into out; false when the shader does not compile.
    bool run(const std::string& field, int dim, const std::vector<float>& points, int seed, std::vector<float>& out) {
        const uint32_t count = static_cast<uint32_t>(points.size() / 4);
        std::string src = "#version 450\nlayout(local_size_x = 64) in;\n" + field +
                          "layout(set = 0, binding = 0) readonly buffer In { vec4 p[]; } i;\n"
                          "layout(set = 0, binding = 1) writeonly buffer Out { float v[]; } o;\n"
                          "layout(push_constant) uniform P { int seed; uint count; } pc;\n"
                          "void main() {\n"
                          "    uint k = gl_GlobalInvocationID.x;\n"
                          "    if (k >= pc.count) return;\n";
        src += dim == 2 ? "    o.v[k] = field(i.p[k].xy, pc.seed);\n" : "    o.v[k] = field(i.p[k].xyz, pc.seed);\n";
        src += "}\n";
        std::string log;
        const auto spirv = render::compileGlslToSpirv(src, render::ShaderStage::Compute, &log);
        if (spirv.empty()) {
            std::printf("    the GLSL does not compile:\n%s\n", log.c_str());
            return false;
        }
        VkDevice d = context.device();
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spirv.size() * 4;
        mi.pCode = spirv.data();
        VkShaderModule module;
        vkCreateShaderModule(d, &mi, nullptr, &module);
        VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2;
        li.pBindings = bindings;
        VkDescriptorSetLayout setLayout;
        vkCreateDescriptorSetLayout(d, &li, nullptr, &setLayout);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        VkPipelineLayout layout;
        vkCreatePipelineLayout(d, &pli, nullptr, &layout);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                    "main", nullptr};
        ci.layout = layout;
        VkPipeline pipeline;
        vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VkDescriptorPool pool;
        vkCreateDescriptorPool(d, &pi, nullptr, &pool);
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = pool;
        sai.descriptorSetCount = 1;
        sai.pSetLayouts = &setLayout;
        VkDescriptorSet set;
        vkAllocateDescriptorSets(d, &sai, &set);

        Buffer in = buffer(points.size() * 4), res = buffer(count * 4);
        std::memcpy(in.mapped, points.data(), points.size() * 4);
        VkDescriptorBufferInfo infos[2] = {{in.buffer, 0, VK_WHOLE_SIZE}, {res.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for (int k = 0; k < 2; ++k) {
            writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[k].dstSet = set;
            writes[k].dstBinding = static_cast<uint32_t>(k);
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[k].pBufferInfo = &infos[k];
        }
        vkUpdateDescriptorSets(d, 2, writes, 0, nullptr);

        VkCommandBuffer cmd = context.beginSingleTimeCommands();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const int32_t push[2] = {seed, static_cast<int32_t>(count)};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push);
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        context.endSingleTimeCommands(cmd);
        out.assign(static_cast<const float*>(res.mapped), static_cast<const float*>(res.mapped) + count);

        destroy(in);
        destroy(res);
        vkDestroyDescriptorPool(d, pool, nullptr);
        vkDestroyPipeline(d, pipeline, nullptr);
        vkDestroyPipelineLayout(d, layout, nullptr);
        vkDestroyDescriptorSetLayout(d, setLayout, nullptr);
        vkDestroyShaderModule(d, module, nullptr);
        return true;
    }
};

// The tree `root` at a few thousand points spread over `extent` about the
// origin, both ways, in 2D and 3D: every point within `tolerance` of the
// field's range but a few (`strays`, as a fraction) where the CPU's relaxed
// maths crossed a cell's edge.
void check(Gpu& gpu, const char* name, Tree& tree, NodeData* root, float extent, bool dims3 = true,
           float tolerance = 2e-4f, double strays = 2e-3) {
    const std::string encoded = Metadata::SerialiseNodeData(root);
    auto cpu = FastNoise::NewFromEncodedNodeTree(encoded.c_str());
    if (encoded.empty() || !cpu) {
        std::printf("FAIL %s: the tree does not encode\n", name);
        ++failures;
        return;
    }
    for (int dim : {2, 3}) {
        if (dim == 3 && !dims3) continue;
        brokit::noise::GlslOptions options;
        options.dims2 = dim == 2;
        options.dims3 = dim == 3;
        std::string glsl, error;
        if (!brokit::noise::treeToGlsl(encoded, options, glsl, error)) {
            std::printf("FAIL %s %dD: %s\n", name, dim, error.c_str());
            ++failures;
            continue;
        }
        // A lattice off the integers, with a few points on them.
        constexpr int kSide = 64;
        const int count = dim == 2 ? kSide * kSide : 16 * 16 * 16;
        std::vector<float> points(static_cast<size_t>(count) * 4, 0.0f), xs(count), ys(count), zs(count);
        uint32_t rng = 12345;
        auto rand01 = [&] {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / 16777216.0f;
        };
        for (int k = 0; k < count; ++k) {
            const bool onGrid = k % 97 == 0;
            for (int a = 0; a < dim; ++a) {
                float v = (rand01() * 2.0f - 1.0f) * extent;
                if (onGrid) v = std::round(v);
                points[static_cast<size_t>(k) * 4 + static_cast<size_t>(a)] = v;
            }
            xs[k] = points[k * 4];
            ys[k] = points[k * 4 + 1];
            zs[k] = points[k * 4 + 2];
        }
        for (int seed : {1337, -7}) {
            // FastNoise2's value at each point (GenSingle: the point in
            // every lane, the first kept), and its batch (GenPositionArray,
            // a point a lane), which should be the same.
            std::vector<float> want(count), batch(count), got;
            for (int k = 0; k < count; ++k)
                want[k] = dim == 2 ? cpu->GenSingle2D(xs[k], ys[k], seed) : cpu->GenSingle3D(xs[k], ys[k], zs[k], seed);
            if (dim == 2) cpu->GenPositionArray2D(batch.data(), count, xs.data(), ys.data(), 0, 0, seed);
            else cpu->GenPositionArray3D(batch.data(), count, xs.data(), ys.data(), zs.data(), 0, 0, 0, seed);
            if (!gpu.run(glsl, dim, points, seed, got)) {
                ++failures;
                return;
            }
            float lo = want[0], hi = want[0];
            for (float v : want) lo = std::min(lo, v), hi = std::max(hi, v);
            const float tol = tolerance * std::max(hi - lo, 1e-6f);
            int over = 0;
            float worst = 0.0f;
            double sum = 0.0;
            int worstAt = 0;
            for (int k = 0; k < count; ++k) {
                const float e = std::isfinite(got[k]) ? std::abs(got[k] - want[k]) : INFINITY;
                if (e > tol) ++over;
                if (e > worst) worst = e, worstAt = k;
                sum += std::min(e, hi - lo);
            }
            // (Not this test's to fail: at the pinned FastNoise2 a
            // Checkerboard's batch disagrees with its own single values at
            // about a quarter of the points; the GLSL follows the single.)
            int batchApart = 0;
            for (int k = 0; k < count; ++k) batchApart += std::abs(batch[k] - want[k]) > tol;
            if (batchApart)
                std::printf("note %s %dD: FastNoise2's batch differs from its single values at %d of %d points\n",
                            name, dim, batchApart, count);
            const bool ok = over <= strays * count;
            std::printf("%s %s %dD seed %d: range [%.4g, %.4g], mean |err| %.3g, worst %.3g at (%g, %g, %g) "
                        "(cpu %.6g gpu %.6g), %d of %d over %.3g\n",
                        ok ? "ok  " : "FAIL", name, dim, seed, lo, hi, sum / count, worst, xs[worstAt], ys[worstAt],
                        dim == 3 ? zs[worstAt] : 0.0f, want[worstAt], got[worstAt], over, count, tol);
            if (!ok) ++failures;
        }
    }
}

}  // namespace

int main() {
    std::printf("=== bro_noise_glsl_test: FastNoise2 trees as GLSL against FastNoise2 ===\n");
    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::fprintf(stderr, "Failed to initialize headless Vulkan context!\n");
        return 1;
    }
    Gpu gpu(context);

    // The coherent noises alone, at their default scale and at others.
    for (const char* type : {"Value", "Perlin", "Simplex"}) {
        Tree t;
        NodeData* n = t.node(type);
        t.var(n, "Feature Scale", 37.0f);
        check(gpu, type, t, n, 2000.0f);
        Tree u;
        NodeData* m = u.node(type);
        u.var(m, "Output Min", 0.0f);
        u.var(m, "Output Max", 90.0f);
        u.var(m, "Seed Offset", 5);
        check(gpu, (std::string(type) + " ranged").c_str(), u, m, 5000.0f);
    }
    {
        Tree t;
        NodeData* n = t.node("White");
        check(gpu, "White", t, n, 500.0f);
    }
    for (const char* type : {"Checkerboard", "SineWave"}) {
        Tree t;
        NodeData* n = t.node(type);
        t.var(n, "Feature Scale", 13.0f);
        check(gpu, type, t, n, 300.0f);
    }
    // The fractals, weighted, and through a domain scale.
    for (const char* type : {"FractalFBm", "FractalRidged"}) {
        Tree t;
        NodeData* f = t.node(type);
        NodeData* s = t.node("Simplex");
        t.src(f, "Source", s);
        t.var(f, "Octaves", 5);
        t.var(f, "Lacunarity", 2.03f);
        t.hyb(f, "Gain", nullptr, 0.55f);
        t.hyb(f, "Weighted Strength", nullptr, 0.3f);
        check(gpu, type, t, f, 3000.0f);
    }
    // biomes' land: two value fBms, scaled and summed.
    {
        Tree t;
        auto fbm = [&](float scale, int octaves, int seedOffset) {
            NodeData* v = t.node("Value");
            t.var(v, "Feature Scale", scale);
            t.var(v, "Seed Offset", seedOffset);
            NodeData* f = t.node("FractalFBm");
            t.src(f, "Source", v);
            t.var(f, "Octaves", octaves);
            t.var(f, "Lacunarity", 2.03f);
            return f;
        };
        NodeData* a = t.node("Remap");
        t.src(a, "Source", fbm(1.0f / 0.0024f, 4, 0));
        t.hyb(a, "To Min", nullptr, -45.0f);
        t.hyb(a, "To Max", nullptr, 45.0f);
        NodeData* add = t.node("Add");
        t.src(add, "LHS", a);
        t.hyb(add, "RHS", fbm(1.0f / 0.009f, 3, 1));
        check(gpu, "land", t, add, 4000.0f);
    }
    // The blends, sources and constants on both sides.
    for (const char* type : {"Add", "Multiply", "Min", "Max", "MinSmooth", "MaxSmooth"}) {
        Tree t;
        NodeData* n = t.node(type);
        NodeData* a = t.node("Perlin");
        NodeData* b = t.node("Simplex");
        t.var(b, "Feature Scale", 41.0f);
        t.src(n, "LHS", a);
        t.hyb(n, "RHS", b);
        if (std::strstr(type, "Smooth")) t.hyb(n, "Smoothness", nullptr, 0.4f);
        check(gpu, type, t, n, 1000.0f);
    }
    for (const char* type : {"Subtract", "Divide", "Modulus", "PowFloat"}) {
        Tree t;
        NodeData* n = t.node(type);
        NodeData* a = t.node("Perlin");
        const char* lhs = std::strcmp(type, "PowFloat") == 0 ? "Value" : "LHS";
        const char* rhs = std::strcmp(type, "PowFloat") == 0 ? "Pow" : "RHS";
        t.hyb(n, lhs, a);
        t.hyb(n, rhs, nullptr, 1.7f);
        check(gpu, type, t, n, 1000.0f);
    }
    {
        Tree t;
        NodeData* n = t.node("PowInt");
        NodeData* a = t.node("Simplex");
        t.src(n, "Value", a);
        t.var(n, "Pow", 3);
        check(gpu, "PowInt", t, n, 1000.0f);
    }
    {
        Tree t;
        NodeData* n = t.node("Fade");
        t.src(n, "A", t.node("Perlin"));
        t.src(n, "B", t.node("Value"));
        t.hyb(n, "Fade", t.node("Simplex"));
        t.var(n, "Interpolation", 2);
        check(gpu, "Fade", t, n, 1000.0f);
    }
    // The generators of position.
    {
        Tree t;
        NodeData* n = t.node("Gradient");
        t.var(n, "Multiplier", 0.5f, 0);
        t.var(n, "Multiplier", -0.25f, 1);
        t.var(n, "Multiplier", 0.125f, 2);
        t.hyb(n, "Offset", t.node("Perlin"), 0.0f, 0);
        t.hyb(n, "Offset", nullptr, 30.0f, 1);
        check(gpu, "Gradient", t, n, 400.0f);
    }
    for (int fn = 0; fn < 6; ++fn) {
        Tree t;
        NodeData* n = t.node("DistanceToPoint");
        t.var(n, "Distance Function", static_cast<float>(fn));
        t.hyb(n, "Point", nullptr, 12.5f, 0);
        t.hyb(n, "Point", nullptr, -40.0f, 1);
        t.hyb(n, "Point", nullptr, 3.0f, 2);
        check(gpu, ("DistanceToPoint " + std::to_string(fn)).c_str(), t, n, 200.0f);
    }
    // The modifiers, each over a noise.
    auto over = [&](const char* type, const std::function<void(Tree&, NodeData*)>& set, bool dims3 = true) {
        Tree t;
        NodeData* n = t.node(type);
        NodeData* s = t.node("Simplex");
        t.var(s, "Feature Scale", 23.0f);
        t.src(n, "Source", s);
        set(t, n);
        check(gpu, type, t, n, 800.0f, dims3);
    };
    over("DomainScale", [](Tree& t, NodeData* n) { t.var(n, "Scaling", 1.7f); });
    over("DomainAxisScale", [](Tree& t, NodeData* n) {
        t.var(n, "Scaling", 1.5f, 0);
        t.var(n, "Scaling", 0.5f, 1);
        t.var(n, "Scaling", 2.0f, 2);
    });
    over("DomainOffset", [](Tree& t, NodeData* n) {
        t.hyb(n, "Offset", nullptr, 17.25f, 0);
        t.hyb(n, "Offset", t.node("Perlin"), 0.0f, 1);
        t.hyb(n, "Offset", nullptr, -3.0f, 2);
    });
    over("DomainRotate", [](Tree& t, NodeData* n) { t.var(n, "Yaw", 0.6f); });
    over("DomainRotate", [](Tree& t, NodeData* n) {
        t.var(n, "Yaw", 0.6f);
        t.var(n, "Pitch", -0.3f);
        t.var(n, "Roll", 1.1f);
    });
    over("SeedOffset", [](Tree& t, NodeData* n) { t.var(n, "Seed Offset", 9); });
    over("Remap", [](Tree& t, NodeData* n) {
        t.hyb(n, "From Min", nullptr, -0.5f);
        t.hyb(n, "To Max", nullptr, 4.0f);
        t.var(n, "Clamp Output", 1);
    });
    over("Terrace", [](Tree& t, NodeData* n) {
        t.var(n, "Step Count", 6.0f);
        t.hyb(n, "Smoothness", nullptr, 0.3f);
    });
    over("PingPong",[](Tree& t, NodeData* n) { t.hyb(n, "Ping Pong Strength", nullptr, 2.5f); });
    over("AddDimension", [](Tree& t, NodeData* n) { t.hyb(n, "New Dimension Position", nullptr, 7.5f); }, false);
    over("RemoveDimension", [](Tree& t, NodeData* n) { t.var(n, "Remove Dimension", 1); });
    over("GeneratorCache", [](Tree&, NodeData*) {});
    over("SignedSquareRoot", [](Tree&, NodeData*) {});
    over("Abs", [](Tree&, NodeData*) {});
    over("DomainRotatePlane", [](Tree&, NodeData*) {});
    over("DomainRotatePlane", [](Tree& t, NodeData* n) { t.var(n, "Rotation Type", 1); });

    // One not ported is refused by name.
    {
        Tree t;
        NodeData* n = t.node("CellularDistance");
        std::string glsl, error;
        const bool ok = brokit::noise::treeToGlsl(Metadata::SerialiseNodeData(n), {}, glsl, error);
        if (ok || error.find("CellularDistance") == std::string::npos) {
            std::printf("FAIL a node not ported is not refused by name (%s)\n", error.c_str());
            ++failures;
        }
    }

    if (failures) {
        std::printf("=== bro_noise_glsl_test: %d check(s) FAILED ===\n", failures);
        return 1;
    }
    std::printf("=== bro_noise_glsl_test: all checks passed ===\n");
    return 0;
}
