#include "bronze_host/host_voxeldag.h"

#include "bronze_host/host_kernel_gpu.h"
#include "bronze_host/host_rooted.h"
#include "bronze_host/native_scene_internal.h"
#include "embed/embed_host_class.h"
#include "embed/embed_object_builder.h"
#include "engine/engine.h"
#include "render/vulkan_compute.h"
#include "scene/nodes/voxel_world_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"

#include <bromath/aabb.h>
#include <bromesh/api.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/voxel/voxel_dag.h>
#include <brokernel/api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct WorldHandle {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t nodeId = 0;

    std::shared_ptr<brokernel::api::GpuBuffer> gpuRegions;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuNodes;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuChildren;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuBricks;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuAttrs;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuAnimClips;
    std::shared_ptr<brokernel::api::GpuBuffer> gpuAnimFrames;
    uint64_t syncedPoolGen = 0;
    uint64_t syncedRegionsGen = 0;
    uint32_t regionCount = 0;
    double syncedAnchor[3] = {0.0, 0.0, 0.0};
};

ev::HostClass g_worldClass;

double numberOr(Value obj, const char* key, double fallback) {
    if (!ev::isObject(obj)) return fallback;
    const Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : fallback;
}

int32_t intOr(Value obj, const char* key, int32_t fallback, int32_t lo, int32_t hi) {
    const double d = numberOr(obj, key, static_cast<double>(fallback));
    if (!std::isfinite(d)) return fallback;
    return std::clamp(static_cast<int32_t>(std::llround(d)), lo, hi);
}

bool boolOr(Value obj, const char* key, bool fallback) {
    if (!ev::isObject(obj)) return fallback;
    const Value v = ev::getProperty(obj, key);
    return ev::isUndefined(v) ? fallback : ev::toBool(v);
}

std::string stringOr(Value obj, const char* key, const char* fallback) {
    if (!ev::isObject(obj)) return fallback;
    const Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : std::string(fallback);
}

void vec3Or(Value obj, const char* key, float out[3]) {
    if (!ev::isObject(obj)) return;
    const Rooted arr(ev::getProperty(obj, key));
    if (!ev::isObject(arr)) return;
    for (uint32_t i = 0; i < 3; ++i) {
        const Value v = ev::getElement(arr, i);
        if (ev::isNumber(v)) out[i] = static_cast<float>(ev::toDouble(v));
    }
}

void vec3DoubleOr(Value obj, const char* key, double out[3]) {
    if (!ev::isObject(obj)) return;
    const Rooted arr(ev::getProperty(obj, key));
    if (!ev::isObject(arr)) return;
    for (uint32_t i = 0; i < 3; ++i) {
        const Value v = ev::getElement(arr, i);
        if (ev::isNumber(v)) out[i] = ev::toDouble(v);
    }
}

bool readFloatVec(Value val, std::vector<float>& out) {
    if (!ev::isObject(val)) return false;
    if (auto info = ev::typedArrayInfo(val); info && info.elementKind == bronze::ElementKind::Float32) {
        const size_t n = info.byteLength / sizeof(float);
        out.resize(n);
        if (n > 0 && info.data) std::memcpy(out.data(), info.data, n * sizeof(float));
        return true;
    }
    const Value lenV = ev::getProperty(val, "length");
    if (!ev::isNumber(lenV)) return false;
    const uint32_t n = static_cast<uint32_t>(std::max(0.0, ev::toDouble(lenV)));
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<float>(ev::toDouble(ev::getElement(val, i)));
    return true;
}

bool readU32Vec(Value val, std::vector<uint32_t>& out) {
    if (!ev::isObject(val)) return false;
    if (auto info = ev::typedArrayInfo(val); info && info.elementKind == bronze::ElementKind::Uint32) {
        const size_t n = info.byteLength / sizeof(uint32_t);
        out.resize(n);
        if (n > 0 && info.data) std::memcpy(out.data(), info.data, n * sizeof(uint32_t));
        return true;
    }
    const Value lenV = ev::getProperty(val, "length");
    if (!ev::isNumber(lenV)) return false;
    const uint32_t n = static_cast<uint32_t>(std::max(0.0, ev::toDouble(lenV)));
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<uint32_t>(std::max(0.0, ev::toDouble(ev::getElement(val, i))));
    return true;
}

WorldHandle* worldOf(Value self, const char* what, scene::VoxelWorldNode** outNode = nullptr, scene::SceneGraph** outGraph = nullptr) {
    auto* w = static_cast<WorldHandle*>(g_worldClass.unwrap(self));
    if (!w) ev::throwTypeError(std::string(what) + ": not a VoxelWorld");
    auto tok = w->token.lock();
    if (!tok || !tok->graph) ev::throwTypeError(std::string(what) + ": scene graph was destroyed");
    scene::SceneNode* n = tok->graph->resolveNode(w->nodeId);
    if (!n || n->type() != scene::SceneNode::Type::VoxelWorld) {
        ev::throwTypeError(std::string(what) + ": voxel world node was destroyed");
    }
    if (outNode) *outNode = static_cast<scene::VoxelWorldNode*>(n);
    if (outGraph) *outGraph = tok->graph;
    return w;
}

bromesh::VoxelState parseStateOpt(Value opts, bromesh::VoxelState fallback) {
    const std::string s = stringOr(opts, "state", "");
    if (s == "carve" || s == "Carve") return bromesh::VoxelState::Carve;
    if (s == "solid" || s == "Solid") return bromesh::VoxelState::Solid;
    return fallback;
}

bromesh::DagVoxelizeOptions parseVoxOpts(Value opts, int depthDefault, bromesh::VoxelState stateDefault) {
    bromesh::DagVoxelizeOptions o;
    const int d = std::clamp(depthDefault, 1, 6);
    o.resolution = 4 << d;
    o.state = parseStateOpt(opts, stateDefault);
    if (ev::isObject(opts)) {
        const Rooted col(ev::getProperty(opts, "color"));
        parseColorValue(col, o.defaultColor[0], o.defaultColor[1], o.defaultColor[2], o.defaultColor[3]);
        o.roughness = static_cast<float>(std::clamp(numberOr(opts, "roughness", 0.8), 0.04, 1.0));
        o.emissive = static_cast<float>(std::clamp(numberOr(opts, "emissive", 0.0), 0.0, 1.0));
        o.fillInterior = boolOr(opts, "fillInterior", false);
        o.fitToCube = boolOr(opts, "fitToCube", true);
        o.unitBounds = boolOr(opts, "unitBounds", false);
    }
    return o;
}

// An archetype is what a script places again and again, so the pool holds it
// whether or not a region reaches it now, until `releaseArchetype`.
Value makeArchetypeResult(scene::VoxelWorldNode* vw, uint32_t ref, bool isLeaf, int depth) {
    vw->pool().pin(ref, isLeaf);
    ev::ObjectBuilder o;
    o.set("ref", static_cast<double>(ref));
    o.set("isLeaf", isLeaf);
    o.set("depth", depth);
    return o.get();
}

void parseRefArg(Value v, uint32_t& outRef, bool& outIsLeaf) {
    if (ev::isNumber(v)) {
        outRef = static_cast<uint32_t>(std::llround(ev::toDouble(v)));
        outIsLeaf = false;
        return;
    }
    if (ev::isObject(v)) {
        outRef = static_cast<uint32_t>(std::llround(numberOr(v, "ref", static_cast<double>(bromesh::kDagNullNode))));
        outIsLeaf = boolOr(v, "isLeaf", false);
        return;
    }
    outRef = bromesh::kDagNullNode;
    outIsLeaf = false;
}

void fillRegionFromOpts(Value opts, scene::VoxelRegion& r, bool isCreate) {
    if (!ev::isObject(opts)) return;
    vec3DoubleOr(opts, "center", r.center);
    vec3Or(opts, "right", r.right);
    vec3Or(opts, "up", r.up);
    vec3Or(opts, "forward", r.forward);
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "voxelSize"))) {
        r.voxelSize = static_cast<float>(std::max(1e-4, numberOr(opts, "voxelSize", r.voxelSize)));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "maxDepth"))) {
        r.maxDepth = static_cast<uint32_t>(intOr(opts, "maxDepth", static_cast<int32_t>(r.maxDepth), 1, 16));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "minDist"))) {
        r.minDist = static_cast<float>(std::clamp(numberOr(opts, "minDist", r.minDist), 0.0,
                                                  static_cast<double>(scene::kVoxelMaxMinDist)));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "minFadeEnd"))) {
        r.minFadeEnd = static_cast<float>(std::max(0.0, numberOr(opts, "minFadeEnd", r.minFadeEnd)));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "maxDist"))) {
        r.maxDist = static_cast<float>(std::max(0.0, numberOr(opts, "maxDist", r.maxDist)));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "fadeStart"))) {
        r.fadeStart = static_cast<float>(std::max(0.0, numberOr(opts, "fadeStart", r.fadeStart)));
    }
    if (isCreate || !ev::isUndefined(ev::getProperty(opts, "animTickOffset"))) {
        r.animTickOffset = static_cast<uint32_t>(std::max(0.0, numberOr(opts, "animTickOffset", r.animTickOffset)));
    }
    const bool enabled = boolOr(opts, "enabled", (r.flags & scene::kVoxelRegionEnabled) != 0u);
    const bool castShadow = boolOr(opts, "castShadow", (r.flags & scene::kVoxelRegionCastShadow) != 0u);
    const bool foliage = boolOr(opts, "foliage", (r.flags & scene::kVoxelRegionFoliage) != 0u);
    r.flags = (enabled ? scene::kVoxelRegionEnabled : 0u) |
              (castShadow ? scene::kVoxelRegionCastShadow : 0u) |
              (foliage ? scene::kVoxelRegionFoliage : 0u) |
              (r.flags & scene::kVoxelRegionHasCarve);
}

std::function<float(float, float, float)> buildAnalyticSdf(Value shapeVal) {
    const std::string kind = stringOr(shapeVal, "kind", "sphere");
    float c[3] = {0.0f, 0.0f, 0.0f};
    vec3Or(shapeVal, "center", c);

    if (kind == "box") {
        float h[3] = {0.88f, 0.88f, 0.88f};
        vec3Or(shapeVal, "half", h);
        return [c, h](float x, float y, float z) {
            const float dx = std::abs(x - c[0]) - h[0];
            const float dy = std::abs(y - c[1]) - h[1];
            const float dz = std::abs(z - c[2]) - h[2];
            const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f), oz = std::max(dz, 0.0f);
            return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(dx, std::max(dy, dz)), 0.0f);
        };
    }
    if (kind == "cylinder" || kind == "tunnel") {
        const float r = static_cast<float>(numberOr(shapeVal, "radius", 0.55));
        const float hl = static_cast<float>(numberOr(shapeVal, "halfLength", kind == "tunnel" ? 1.2 : 0.9));
        const std::string axis = stringOr(shapeVal, "axis", kind == "tunnel" ? "z" : "y");
        return [c, r, hl, axis](float x, float y, float z) {
            const float px = x - c[0], py = y - c[1], pz = z - c[2];
            float rad = 0.0f, ax = 0.0f;
            if (axis == "x") { rad = std::sqrt(py * py + pz * pz); ax = std::abs(px); }
            else if (axis == "z") { rad = std::sqrt(px * px + py * py); ax = std::abs(pz); }
            else { rad = std::sqrt(px * px + pz * pz); ax = std::abs(py); }
            const float d1 = rad - r, d2 = ax - hl;
            return std::min(std::max(d1, d2), 0.0f) +
                   std::sqrt(std::max(d1, 0.0f) * std::max(d1, 0.0f) + std::max(d2, 0.0f) * std::max(d2, 0.0f));
        };
    }
    if (kind == "capsule") {
        float a[3] = {0.0f, -0.6f, 0.0f}, b[3] = {0.0f, 0.6f, 0.0f};
        vec3Or(shapeVal, "a", a);
        vec3Or(shapeVal, "b", b);
        const float r = static_cast<float>(numberOr(shapeVal, "radius", 0.35));
        return [a, b, r](float x, float y, float z) {
            const float pax = x - a[0], pay = y - a[1], paz = z - a[2];
            const float bax = b[0] - a[0], bay = b[1] - a[1], baz = b[2] - a[2];
            const float dotBA = bax * bax + bay * bay + baz * baz;
            const float h = dotBA > 1e-8f ? std::clamp((pax * bax + pay * bay + paz * baz) / dotBA, 0.0f, 1.0f) : 0.0f;
            const float dx = pax - bax * h, dy = pay - bay * h, dz = paz - baz * h;
            return std::sqrt(dx * dx + dy * dy + dz * dz) - r;
        };
    }
    if (kind == "tree") {
        const float crownR = static_cast<float>(numberOr(shapeVal, "crownRadius", 0.62));
        const float trunkR = static_cast<float>(numberOr(shapeVal, "trunkRadius", 0.18));
        return [c, crownR, trunkR](float x, float y, float z) {
            const float px = x - c[0], py = y - c[1], pz = z - c[2];
            const float dCrown = std::sqrt(px * px + (py - 0.25f) * (py - 0.25f) + pz * pz) - crownR;
            const float td = std::sqrt(px * px + pz * pz) - trunkR;
            const float ty = std::abs(py + 0.45f) - 0.45f;
            const float dTrunk = std::max(td, ty);
            return std::min(dCrown, dTrunk);
        };
    }
    const float r = static_cast<float>(numberOr(shapeVal, "radius", 0.85));
    return [c, r](float x, float y, float z) {
        const float dx = x - c[0], dy = y - c[1], dz = z - c[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) - r;
    };
}

template <typename T>
bool syncOneBuffer(render::VulkanCompute* comp,
                   std::shared_ptr<brokernel::api::GpuBuffer>& dst,
                   const std::vector<T>& src,
                   std::string* err) {
    const size_t byteSize = src.empty() ? size_t{64} : src.size() * sizeof(T);
    auto cb = dst ? computeBufferOf(dst) : nullptr;
    if (!cb || cb->size() < byteSize) {
        dst = createEngineBuffer(byteSize, err);
        if (!dst) return false;
        cb = computeBufferOf(dst);
        if (!cb) {
            if (err) *err = "failed to resolve ComputeBuffer";
            return false;
        }
    }
    if (src.empty()) {
        const uint8_t zero[64] = {};
        return comp->write(*cb, 0, zero, sizeof(zero));
    }
    return comp->write(*cb, 0, src.data(), byteSize);
}

// A paged pool array, its pages gathered into one copy.
template <typename T>
bool syncOneBuffer(render::VulkanCompute* comp,
                   std::shared_ptr<brokernel::api::GpuBuffer>& dst,
                   const bromesh::VoxelDagPool::Paged<T>& src,
                   std::string* err) {
    const size_t byteSize = src.empty() ? size_t{64} : src.size() * sizeof(T);
    auto cb = dst ? computeBufferOf(dst) : nullptr;
    if (!cb || cb->size() < byteSize) {
        dst = createEngineBuffer(byteSize, err);
        if (!dst) return false;
        cb = computeBufferOf(dst);
        if (!cb) {
            if (err) *err = "failed to resolve ComputeBuffer";
            return false;
        }
    }
    if (src.empty()) {
        const uint8_t zero[64] = {};
        return comp->write(*cb, 0, zero, sizeof(zero));
    }
    return comp->write(*cb, 0, byteSize, [&](uint8_t* out) {
        for (size_t p = 0; p < src.pageCount(); ++p) {
            const size_t n = src.pageSize(p) * sizeof(T);
            std::memcpy(out, src.page(p), n);
            out += n;
        }
    });
}

void parseAnchorArg(std::span<const Value> a, double out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    if (a.empty() || !ev::isObject(a[0])) return;
    const Rooted arr(a[0]);
    for (uint32_t i = 0; i < 3; ++i) {
        const Value v = ev::getElement(arr, i);
        if (ev::isNumber(v)) out[i] = ev::toDouble(v);
    }
}

bool syncWorldBuffers(WorldHandle* w, scene::VoxelWorldNode* vw, const double anchor[3], std::string* err) {
    render::VulkanCompute* comp = engineCompute();
    if (!comp || !comp->available()) {
        if (err) *err = "VulkanCompute not available";
        return false;
    }

    const auto& pool = vw->pool();
    if (w->syncedPoolGen != pool.generation() || !w->gpuNodes) {
        if (!syncOneBuffer(comp, w->gpuNodes, pool.nodes(), err) ||
            !syncOneBuffer(comp, w->gpuChildren, pool.children(), err) ||
            !syncOneBuffer(comp, w->gpuBricks, pool.bricks(), err) ||
            !syncOneBuffer(comp, w->gpuAttrs, pool.attrs(), err) ||
            !syncOneBuffer(comp, w->gpuAnimClips, pool.animClips(), err) ||
            !syncOneBuffer(comp, w->gpuAnimFrames, pool.animFrames(), err)) {
            return false;
        }
        w->syncedPoolGen = pool.generation();
    }

    const bool anchorChanged =
        w->syncedAnchor[0] != anchor[0] ||
        w->syncedAnchor[1] != anchor[1] ||
        w->syncedAnchor[2] != anchor[2];
    if (w->syncedRegionsGen != vw->regionsGeneration() || !w->gpuRegions || anchorChanged) {
        std::vector<scene::GpuVoxelRegion> gpuRegs;
        gpuRegs.reserve(vw->regions().size());
        for (const auto& reg : vw->regions()) {
            if ((reg.flags & scene::kVoxelRegionEnabled) == 0u || reg.rootNode == bromesh::kDagNullNode) continue;
            gpuRegs.push_back(reg.packGpu(anchor));
        }
        if (!syncOneBuffer(comp, w->gpuRegions, gpuRegs, err)) return false;
        w->regionCount = static_cast<uint32_t>(gpuRegs.size());
        w->syncedRegionsGen = vw->regionsGeneration();
        w->syncedAnchor[0] = anchor[0];
        w->syncedAnchor[1] = anchor[1];
        w->syncedAnchor[2] = anchor[2];
    }

    comp->flush();
    return true;
}

Value buildSdfArchetype(Value self, std::span<const Value> args, bromesh::VoxelState defaultState) {
    scene::VoxelWorldNode* vw = nullptr;
    worldOf(self, "VoxelWorld.addArchetypeFromSdf", &vw);
    if (args.empty()) return ev::throwTypeError("addArchetypeFromSdf: shape or callback required");

    const Rooted shapeOrFn(args[0]);
    const int depth = args.size() >= 2 && ev::isNumber(args[1]) ? std::clamp(static_cast<int>(ev::toDouble(args[1])), 1, 6) : 2;
    const Rooted opts(args.size() >= 3 ? args[2] : ev::undefined());
    const bromesh::DagVoxelizeOptions dagOpts = parseVoxOpts(opts, depth, defaultState);
    const bromath::AABB3 box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};

    int outDepth = 0;
    uint32_t ref = bromesh::kDagNullNode;
    if (ev::isFunction(shapeOrFn)) {
        const auto sdfCb = [&](float x, float y, float z) -> float {
            Value argv[3] = {ev::fromDouble(x), ev::fromDouble(y), ev::fromDouble(z)};
            const ev::CallResult r = ev::call(shapeOrFn, ev::undefined(), argv);
            if (r.thrown) ev::throwValue(r.value);
            return ev::isNumber(r.value) ? static_cast<float>(ev::toDouble(r.value)) : 1.0f;
        };
        ref = bromesh::voxelizeSdfToDag(vw->pool(), sdfCb, box, dagOpts, &outDepth);
    } else if (ev::isObject(shapeOrFn)) {
        auto fn = buildAnalyticSdf(shapeOrFn);
        ref = bromesh::voxelizeSdfToDag(vw->pool(), fn, box, dagOpts, &outDepth);
    } else {
        return ev::throwTypeError("addArchetypeFromSdf: expected shape object or (x,y,z)=>dist function");
    }

    vw->bumpRegionsGeneration();
    return makeArchetypeResult(vw, ref, false, outDepth);
}

bool readMeshArg(Value meshVal, bromesh::MeshData& mesh);

Value js_addArchetypeFromMesh(Value self, std::span<const Value> args) {
    scene::VoxelWorldNode* vw = nullptr;
    worldOf(self, "VoxelWorld.addArchetypeFromMesh", &vw);
    if (args.empty() || !ev::isObject(args[0])) {
        return ev::throwTypeError("addArchetypeFromMesh: expected Mesh or { positions, indices }");
    }
    const Rooted meshVal(args[0]);
    bromesh::MeshData mesh;
    if (!readMeshArg(meshVal, mesh)) {
        return ev::throwTypeError("addArchetypeFromMesh: positions and indices required");
    }
    const int depth = args.size() >= 2 && ev::isNumber(args[1]) ? std::clamp(static_cast<int>(ev::toDouble(args[1])), 1, 6) : 2;
    const Rooted opts(args.size() >= 3 ? args[2] : ev::undefined());
    const bromesh::DagVoxelizeOptions dagOpts = parseVoxOpts(opts, depth, bromesh::VoxelState::Solid);
    int outDepth = 0;
    const uint32_t ref = bromesh::voxelizeMeshToDag(vw->pool(), mesh, dagOpts, &outDepth);
    vw->bumpRegionsGeneration();
    return makeArchetypeResult(vw, ref, false, outDepth);
}

// ── Voxelizing off the main thread ──────────────────────────────────────────
// A mesh is copied and voxelized into a brick set (bromesh: no pool) on one
// worker thread; the job's take() interns the set into the world's pool on the
// main thread, which costs what the bricks and nodes are, not the triangles.

struct VoxelizeJob {
    bromesh::MeshData mesh;
    bromesh::DagVoxelizeOptions opts;
    bromesh::DagBrickSet set;
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
};

class VoxelizeWorker {
public:
    ~VoxelizeWorker() { stop(); }

    void push(std::shared_ptr<VoxelizeJob> job) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_) return;
            queue_.push_back(std::move(job));
            if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
            queue_.clear();
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = false;
    }

private:
    void run() {
        for (;;) {
            std::shared_ptr<VoxelizeJob> job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!job->cancelled.load(std::memory_order_acquire)) {
                job->set = bromesh::voxelizeMeshToBricks(job->mesh, job->opts);
            }
            job->mesh = {};
            job->done.store(true, std::memory_order_release);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<VoxelizeJob>> queue_;
    std::thread thread_;
    bool stopping_ = false;
};

VoxelizeWorker& voxelizeWorker() {
    static VoxelizeWorker w;
    return w;
}

// The pool of a world named by its scene graph's token and node id, or null
// once either is gone (no throw: a finalizer asks too).
bromesh::VoxelDagPool* poolOf(const std::weak_ptr<scene::SceneGraph::LivenessToken>& token, uint32_t nodeId) {
    auto tok = token.lock();
    if (!tok || !tok->graph) return nullptr;
    scene::SceneNode* n = tok->graph->resolveNode(nodeId);
    if (!n || n->type() != scene::SceneNode::Type::VoxelWorld) return nullptr;
    return &static_cast<scene::VoxelWorldNode*>(n)->pool();
}

struct JobHandle {
    std::shared_ptr<VoxelizeJob> job;
    Rooted world{bronze::Value{}};    // the VoxelWorld it commits into
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t nodeId = 0;
    // A commit take() has begun and not finished: it holds what it interned.
    std::unique_ptr<bromesh::DagBrickCommit> commit;
    bool taken = false;

    void dropCommit() {
        if (!commit) return;
        if (auto* pool = poolOf(token, nodeId)) commit->release(*pool);
        commit.reset();
    }
    ~JobHandle() {
        if (job) job->cancelled.store(true, std::memory_order_release);
        dropCommit();
    }
};

ev::HostClass g_jobClass;

bool readMeshArg(Value meshVal, bromesh::MeshData& mesh) {
    if (const auto* md = bromesh::api::meshDataOf(meshVal)) {
        mesh = *md;
        return true;
    }
    if (!readFloatVec(ev::getProperty(meshVal, "positions"), mesh.positions) ||
        !readU32Vec(ev::getProperty(meshVal, "indices"), mesh.indices)) {
        return false;
    }
    readFloatVec(ev::getProperty(meshVal, "normals"), mesh.normals);
    readFloatVec(ev::getProperty(meshVal, "colors"), mesh.colors);
    if (mesh.normals.empty() && !mesh.positions.empty()) bromesh::computeNormals(mesh);
    return true;
}

Value js_addArchetypeFromMeshLater(Value self, std::span<const Value> args) {
    scene::VoxelWorldNode* vw = nullptr;
    worldOf(self, "VoxelWorld.addArchetypeFromMeshLater", &vw);
    if (args.empty() || !ev::isObject(args[0])) {
        return ev::throwTypeError("addArchetypeFromMeshLater: expected Mesh or { positions, indices }");
    }
    auto job = std::make_shared<VoxelizeJob>();
    if (!readMeshArg(args[0], job->mesh)) {
        return ev::throwTypeError("addArchetypeFromMeshLater: positions and indices required");
    }
    const int depth = args.size() >= 2 && ev::isNumber(args[1]) ? std::clamp(static_cast<int>(ev::toDouble(args[1])), 1, 6) : 2;
    const Rooted opts(args.size() >= 3 ? args[2] : ev::undefined());
    job->opts = parseVoxOpts(opts, depth, bromesh::VoxelState::Solid);
    voxelizeWorker().push(job);
    auto h = std::make_unique<JobHandle>();
    h->job = std::move(job);
    h->world.set(self);
    const auto* wh = static_cast<WorldHandle*>(g_worldClass.unwrap(self));
    h->token = wh->token;
    h->nodeId = wh->nodeId;
    return g_jobClass.createInstance(std::move(h));
}

void decorateJobClass(ev::ObjectBuilder& b) {
    b.accessor("ready", [](Value self, std::span<const Value>) -> Value {
        auto* h = static_cast<JobHandle*>(g_jobClass.unwrap(self));
        return ev::fromBool(h && h->job && h->job->done.load(std::memory_order_acquire));
    });
    // The archetype, interned into the pool (null before `ready`, or once
    // taken). With { budgetMs } the interning stops when that time is spent
    // and take() is null; the next take() goes on from there.
    b.def("take", 1, [](Value self, std::span<const Value> args) -> Value {
        auto* h = static_cast<JobHandle*>(g_jobClass.unwrap(self));
        if (!h || !h->job || h->taken || !h->job->done.load(std::memory_order_acquire)) return ev::null();
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(h->world, "VoxelizeJob.take", &vw);
        double budgetMs = -1.0;
        if (!args.empty() && ev::isObject(args[0])) {
            const Value b = ev::getProperty(args[0], "budgetMs");
            if (ev::isNumber(b)) budgetMs = std::max(0.0, ev::toDouble(b));
        }
        if (!h->commit) h->commit = std::make_unique<bromesh::DagBrickCommit>(std::move(h->job->set));
        const auto t0 = std::chrono::steady_clock::now();
        // A slice is a few hundred interns, a few tenths of a millisecond.
        while (!h->commit->step(vw->pool(), 256)) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (budgetMs >= 0.0 && ms >= budgetMs) return ev::null();
        }
        h->taken = true;
        const uint32_t ref = h->commit->root();
        const int depth = h->commit->depth();
        vw->bumpRegionsGeneration();
        const Value out = makeArchetypeResult(vw, ref, false, depth);   // the world's hold
        h->dropCommit();
        return out;
    });
    b.def("cancel", 0, [](Value self, std::span<const Value>) -> Value {
        auto* h = static_cast<JobHandle*>(g_jobClass.unwrap(self));
        if (h && h->job) h->job->cancelled.store(true, std::memory_order_release);
        if (h) {
            h->taken = true;
            h->dropCommit();
        }
        return ev::undefined();
    });
}

Value js_addArchetypeFromVox(Value self, std::span<const Value> args) {
    scene::VoxelWorldNode* vw = nullptr;
    worldOf(self, "VoxelWorld.addArchetypeFromVox", &vw);
    if (args.empty()) return ev::throwTypeError("addArchetypeFromVox: expected Uint8Array or ArrayBuffer");
    const Rooted bufVal(args[0]);
    std::vector<uint8_t> bytes;
    if (auto ti = ev::typedArrayInfo(bufVal); ti && ti.data) {
        bytes.resize(ti.byteLength);
        std::memcpy(bytes.data(), ti.data, ti.byteLength);
    } else if (auto ab = ev::arrayBufferInfo(bufVal); ab && ab.data) {
        bytes.resize(ab.byteLength);
        std::memcpy(bytes.data(), ab.data, ab.byteLength);
    } else {
        return ev::throwTypeError("addArchetypeFromVox: expected Uint8Array or ArrayBuffer");
    }
    const bromesh::DagVoxelizeOptions opts = parseVoxOpts(args.size() >= 2 ? args[1] : ev::undefined(), 2, bromesh::VoxelState::Solid);
    int outDepth = 0;
    const uint32_t ref = bromesh::loadVoxToDag(vw->pool(), bytes.data(), bytes.size(), &outDepth, opts.roughness);
    vw->bumpRegionsGeneration();
    return makeArchetypeResult(vw, ref, false, outDepth);
}

Value js_addArchetypeFromBox(Value self, std::span<const Value> args) {
    scene::VoxelWorldNode* vw = nullptr;
    worldOf(self, "VoxelWorld.addArchetypeFromBox", &vw);

    float rgba[4] = {0.75f, 0.75f, 0.75f, 1.0f};
    if (!args.empty()) {
        const Rooted cval(args[0]);
        parseColorValue(cval, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    const int depth = args.size() >= 2 && ev::isNumber(args[1]) ? std::clamp(static_cast<int>(ev::toDouble(args[1])), 0, 4) : 1;
    const Rooted opts(args.size() >= 3 ? args[2] : ev::undefined());
    const bromesh::VoxelState state = parseStateOpt(opts, bromesh::VoxelState::Solid);
    const float roughness = static_cast<float>(std::clamp(numberOr(opts, "roughness", 0.8), 0.04, 1.0));
    const float emissive = static_cast<float>(std::clamp(numberOr(opts, "emissive", 0.0), 0.0, 1.0));

    if (depth == 0) {
        uint32_t ref = bromesh::kDagNullNode;
        if (state == bromesh::VoxelState::Carve) {
            ref = vw->pool().internBrick(0u, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu, {});
        } else {
            const uint32_t cPack = bromesh::dag_pack::packRgba8(rgba[0], rgba[1], rgba[2], rgba[3]);
            std::vector<bromesh::DagVoxelAttr> vattrs;
            vattrs.reserve(64);
            for (int lz = 0; lz < 4; ++lz) {
                for (int ly = 0; ly < 4; ++ly) {
                    for (int lx = 0; lx < 4; ++lx) {
                        float nx = (lx == 0) ? -1.0f : (lx == 3 ? 1.0f : 0.0f);
                        float ny = (ly == 0) ? -1.0f : (ly == 3 ? 1.0f : 0.0f);
                        float nz = (lz == 0) ? -1.0f : (lz == 3 ? 1.0f : 0.0f);
                        if (nx == 0.0f && ny == 0.0f && nz == 0.0f) ny = 1.0f;
                        vattrs.push_back({cPack, bromesh::dag_pack::packNormalMat(nx, ny, nz, roughness, emissive)});
                    }
                }
            }
            ref = vw->pool().internBrick(0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u, vattrs);
        }
        vw->bumpRegionsGeneration();
        return makeArchetypeResult(vw, ref, true, 0);
    }

    bromesh::DagVoxelizeOptions dagOpts;
    dagOpts.resolution = 4 << depth;
    dagOpts.state = state;
    dagOpts.roughness = roughness;
    dagOpts.fillInterior = true;
    for (int k = 0; k < 4; ++k) dagOpts.defaultColor[k] = rgba[k];
    int outDepth = 0;
    const uint32_t ref = bromesh::voxelizeSdfToDag(
        vw->pool(),
        [](float x, float y, float z) {
            return std::max(std::abs(x), std::max(std::abs(y), std::abs(z))) - 0.92f;
        },
        bromath::AABB3{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        dagOpts,
        &outDepth);
    vw->bumpRegionsGeneration();
    return makeArchetypeResult(vw, ref, false, outDepth);
}

Value js_createWorld(Value, std::span<const Value> args) {
    if (args.empty()) return ev::throwTypeError("bro.voxeldag.createWorld: expected SceneGraph");
    const Rooted sgVal(args[0]);
    scene::SceneGraph* sg = sceneGraphOf(sgVal);
    if (!sg) return ev::throwTypeError("bro.voxeldag.createWorld: argument is not a valid SceneGraph");

    scene::VoxelWorldNode* vw = sg->createVoxelWorld("voxel_world");
    sg->root()->addChild(vw);

    auto h = std::make_unique<WorldHandle>();
    h->token = sg->livenessToken();
    h->nodeId = vw->id();
    return g_worldClass.createInstance(std::move(h));
}

void decorateWorldClass(ev::ObjectBuilder& b) {
    b.def("addArchetypeFromMesh", 3, js_addArchetypeFromMesh);
    b.def("addArchetypeFromMeshLater", 3, js_addArchetypeFromMeshLater);
    b.def("addArchetypeFromVox", 2, js_addArchetypeFromVox);
    b.def("addArchetypeFromBox", 3, js_addArchetypeFromBox);
    b.def("addArchetypeFromSdf", 3, [](Value self, std::span<const Value> a) {
        return buildSdfArchetype(self, a, bromesh::VoxelState::Solid);
    });
    b.def("addCarveArchetypeFromSdf", 3, [](Value self, std::span<const Value> a) {
        return buildSdfArchetype(self, a, bromesh::VoxelState::Carve);
    });

    b.def("registerAnimClip", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.registerAnimClip", &vw);
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("registerAnimClip: expected array");
        const Rooted arr(a[0]);
        const uint32_t len = static_cast<uint32_t>(std::max(0.0, numberOr(arr, "length", 0.0)));
        std::vector<uint32_t> frames;
        frames.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            uint32_t r = bromesh::kDagNullNode;
            bool leaf = false;
            parseRefArg(ev::getElement(arr, i), r, leaf);
            frames.push_back(r);
        }
        return ev::fromDouble(static_cast<double>(vw->pool().registerAnimClip(frames)));
    });

    b.def("addRegion", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.addRegion", &vw);
        scene::VoxelRegion r;
        if (!a.empty()) fillRegionFromOpts(a[0], r, true);
        return ev::fromDouble(static_cast<double>(vw->addRegion(r)));
    });

    b.def("updateRegion", 2, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.updateRegion", &vw);
        if (a.size() < 2) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const scene::VoxelRegion* cur = vw->findRegion(rid);
        if (!cur) return ev::fromBool(false);
        scene::VoxelRegion updated = *cur;
        fillRegionFromOpts(a[1], updated, false);
        return ev::fromBool(vw->updateRegion(rid, updated));
    });

    b.def("removeRegion", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.removeRegion", &vw);
        if (a.empty()) return ev::fromBool(false);
        return ev::fromBool(vw->removeRegion(static_cast<uint32_t>(std::llround(ev::toDouble(a[0])))));
    });

    b.def("place", 6, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.place", &vw);
        if (a.size() < 6) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const int ix = static_cast<int>(std::llround(ev::toDouble(a[1])));
        const int iy = static_cast<int>(std::llround(ev::toDouble(a[2])));
        const int iz = static_cast<int>(std::llround(ev::toDouble(a[3])));
        uint32_t ref = bromesh::kDagNullNode;
        bool isLeaf = false;
        parseRefArg(a[4], ref, isLeaf);
        // Depth 0 is the region's root: an archetype as deep as the region fills it.
        const uint32_t td = static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[5]))), 0, 16));
        if (td == 0 && (ix != 0 || iy != 0 || iz != 0)) return ev::fromBool(false);
        return ev::fromBool(vw->placeSubtree(rid, ix, iy, iz, ref, isLeaf, td));
    });

    b.def("placeBatch", 3, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.placeBatch", &vw);
        if (a.size() < 3 || !ev::isObject(a[1])) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const Rooted list(a[1]);
        const uint32_t td = static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[2]))), 1, 16));
        const uint32_t len = static_cast<uint32_t>(std::max(0.0, numberOr(list, "length", 0.0)));
        std::vector<bromesh::DagPlacement> batch;
        batch.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            const Rooted item(ev::getElement(list, i));
            if (!ev::isObject(item)) continue;
            bromesh::DagPlacement p;
            p.ix = intOr(item, "ix", 0, 0, 65535);
            p.iy = intOr(item, "iy", 0, 0, 65535);
            p.iz = intOr(item, "iz", 0, 0, 65535);
            parseRefArg(ev::getProperty(item, "archetype"), p.childRef, p.isLeaf);
            if (p.childRef == bromesh::kDagNullNode) {
                p.childRef = static_cast<uint32_t>(std::llround(numberOr(item, "ref", static_cast<double>(bromesh::kDagNullNode))));
                p.isLeaf = boolOr(item, "isLeaf", false);
            }
            batch.push_back(p);
        }
        return ev::fromBool(vw->placeSubtreesBatch(rid, batch, td));
    });

    b.def("placeAnimated", 8, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.placeAnimated", &vw);
        if (a.size() < 8) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const int ix = static_cast<int>(std::llround(ev::toDouble(a[1])));
        const int iy = static_cast<int>(std::llround(ev::toDouble(a[2])));
        const int iz = static_cast<int>(std::llround(ev::toDouble(a[3])));
        const uint32_t clip = static_cast<uint32_t>(std::llround(ev::toDouble(a[4])));
        const uint32_t phase = static_cast<uint32_t>(std::llround(ev::toDouble(a[5])));
        const bool isLeaf = ev::toBool(a[6]);
        const uint32_t td = static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[7]))), 1, 16));
        return ev::fromBool(vw->placeAnimatedSubtree(rid, ix, iy, iz, clip, phase, isLeaf, td));
    });

    b.def("stampCarveAndSolid", 7, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.stampCarveAndSolid", &vw);
        if (a.size() < 7) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const int ix = static_cast<int>(std::llround(ev::toDouble(a[1])));
        const int iy = static_cast<int>(std::llround(ev::toDouble(a[2])));
        const int iz = static_cast<int>(std::llround(ev::toDouble(a[3])));
        uint32_t cRef = bromesh::kDagNullNode, sRef = bromesh::kDagNullNode;
        bool cLeaf = false, sLeaf = false;
        parseRefArg(a[4], cRef, cLeaf);
        parseRefArg(a[5], sRef, sLeaf);
        const uint32_t td = static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[6]))), 1, 16));
        return ev::fromBool(vw->stampCarveAndSolid(rid, ix, iy, iz, cRef, cLeaf, sRef, sLeaf, td));
    });

    b.def("clear", 5, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.clear", &vw);
        if (a.size() < 5) return ev::fromBool(false);
        return ev::fromBool(vw->clearCell(
            static_cast<uint32_t>(std::llround(ev::toDouble(a[0]))),
            static_cast<int>(std::llround(ev::toDouble(a[1]))),
            static_cast<int>(std::llround(ev::toDouble(a[2]))),
            static_cast<int>(std::llround(ev::toDouble(a[3]))),
            static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[4]))), 1, 16))));
    });

    b.def("setAnimTick", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.setAnimTick", &vw);
        if (!a.empty()) vw->setAnimTick(static_cast<uint32_t>(std::max(0.0, ev::toDouble(a[0]))));
        return ev::undefined();
    });
    b.def("setAnimFps", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.setAnimFps", &vw);
        if (!a.empty()) vw->setAnimFps(static_cast<float>(ev::toDouble(a[0])));
        return ev::undefined();
    });
    b.def("setAutoAnimate", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.setAutoAnimate", &vw);
        if (!a.empty()) vw->setAutoAnimate(ev::toBool(a[0]));
        return ev::undefined();
    });
    b.def("setLodScale", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.setLodScale", &vw);
        if (!a.empty()) vw->setLodScale(static_cast<float>(ev::toDouble(a[0])));
        return ev::undefined();
    });
    b.def("setMaxRaySteps", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.setMaxRaySteps", &vw);
        if (!a.empty()) vw->setMaxRaySteps(static_cast<uint32_t>(std::max(1.0, ev::toDouble(a[0]))));
        return ev::undefined();
    });

    b.def("stats", 0, [](Value self, std::span<const Value>) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.stats", &vw);
        const auto& p = vw->pool();
        ev::ObjectBuilder o;
        o.set("nodes", static_cast<double>(p.nodes().size()));
        o.set("liveNodes", static_cast<double>(p.liveNodes()));
        o.set("liveBricks", static_cast<double>(p.liveBricks()));
        o.set("children", static_cast<double>(p.children().size()));
        o.set("bricks", static_cast<double>(p.bricks().size()));
        o.set("attrs", static_cast<double>(p.attrs().size()));
        o.set("animClips", static_cast<double>(p.animClips().size()));
        o.set("animFrames", static_cast<double>(p.animFrames().size()));
        o.set("regions", static_cast<double>(vw->regions().size()));
        o.set("generation", static_cast<double>(p.generation()));
        o.set("animTick", static_cast<double>(vw->animTick()));
        return o.get();
    });

    b.def("mergeArchetypes", 2, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.mergeArchetypes", &vw);
        if (a.size() < 2) return ev::throwTypeError("mergeArchetypes: expected two archetypes");
        uint32_t bRef = bromesh::kDagNullNode, oRef = bromesh::kDagNullNode;
        bool bLeaf = false, oLeaf = false;
        parseRefArg(a[0], bRef, bLeaf);
        parseRefArg(a[1], oRef, oLeaf);
        const int bDepth = ev::isObject(a[0]) ? intOr(a[0], "depth", 2, 0, 16) : 2;
        const int oDepth = ev::isObject(a[1]) ? intOr(a[1], "depth", 2, 0, 16) : 2;
        const uint32_t merged = vw->pool().mergeSubtrees(bRef, bLeaf, oRef, oLeaf);
        vw->bumpRegionsGeneration();
        return makeArchetypeResult(vw, merged, bLeaf && oLeaf, std::max(bDepth, oDepth));
    });

    b.def("makeAnimRef", 2, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.makeAnimRef", &vw);
        if (a.empty()) return ev::throwTypeError("makeAnimRef: clip index required");
        const uint32_t clip = static_cast<uint32_t>(std::max(0.0, ev::toDouble(a[0])));
        const uint32_t phase = a.size() >= 2 ? static_cast<uint32_t>(std::max(0.0, ev::toDouble(a[1]))) : 0u;
        return makeArchetypeResult(vw, vw->pool().makeAnimChildRef(clip, phase), false, 0);
    });

    // Frees what no region root nor archetype reaches; their slots are reused,
    // so a world rebuilding rings on the move stays at its working size.
    b.def("collect", 0, [](Value self, std::span<const Value>) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.collect", &vw);
        std::vector<uint32_t> roots;
        for (const scene::VoxelRegion& r : vw->regions()) roots.push_back(r.rootNode);
        return ev::fromDouble(static_cast<double>(vw->pool().collect(roots)));
    });

    // Drop the hold an archetype was made with: once no region places it, what
    // only it reached is freed (placing it after is an error).
    b.def("releaseArchetype", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.releaseArchetype", &vw);
        if (a.empty()) return ev::fromBool(false);
        uint32_t ref = bromesh::kDagNullNode;
        bool isLeaf = false;
        parseRefArg(a[0], ref, isLeaf);
        vw->pool().release(ref, isLeaf);
        return ev::fromBool(true);
    });

    b.def("clearRegion", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.clearRegion", &vw);
        if (a.empty()) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        return ev::fromBool(vw->setRegionRoot(rid, bromesh::kDagNullNode));
    });

    b.def("placeBatchTyped", 4, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        worldOf(self, "VoxelWorld.placeBatchTyped", &vw);
        if (a.size() < 3 || !ev::isObject(a[1])) return ev::fromBool(false);
        const uint32_t rid = static_cast<uint32_t>(std::llround(ev::toDouble(a[0])));
        const auto info = ev::typedArrayInfo(a[1]);
        if (!info || !info.data ||
            (info.elementKind != bronze::ElementKind::Uint32 && info.elementKind != bronze::ElementKind::Int32)) {
            return ev::throwTypeError("placeBatchTyped: expected Uint32Array or Int32Array of stride 5");
        }
        const uint32_t td = static_cast<uint32_t>(std::clamp(static_cast<int>(std::llround(ev::toDouble(a[2]))), 1, 16));
        const size_t maxItems = info.byteLength / (5u * sizeof(uint32_t));
        const size_t count = (a.size() >= 4 && ev::isNumber(a[3]))
            ? std::min(maxItems, static_cast<size_t>(std::max(0.0, ev::toDouble(a[3]))))
            : maxItems;
        const auto* words = reinterpret_cast<const uint32_t*>(info.data);
        std::vector<bromesh::DagPlacement> batch(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t o = i * 5u;
            batch[i].ix = static_cast<int>(static_cast<int32_t>(words[o + 0u]));
            batch[i].iy = static_cast<int>(static_cast<int32_t>(words[o + 1u]));
            batch[i].iz = static_cast<int>(static_cast<int32_t>(words[o + 2u]));
            batch[i].childRef = words[o + 3u];
            batch[i].isLeaf = words[o + 4u] != 0u;
        }
        return ev::fromBool(vw->placeSubtreesBatch(rid, batch, td));
    });

    b.def("syncBuffers", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        WorldHandle* w = worldOf(self, "VoxelWorld.syncBuffers", &vw);
        double anchor[3] = {0.0, 0.0, 0.0};
        parseAnchorArg(a, anchor);
        std::string err;
        if (!syncWorldBuffers(w, vw, anchor, &err)) return ev::throwError("VoxelWorld.syncBuffers: " + err);
        ev::ObjectBuilder o;
        o.set("synced", true);
        o.set("regionCount", static_cast<double>(w->regionCount));
        return o.get();
    });

    b.def("buffers", 1, [](Value self, std::span<const Value> a) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        WorldHandle* w = worldOf(self, "VoxelWorld.buffers", &vw);
        double anchor[3] = {0.0, 0.0, 0.0};
        parseAnchorArg(a, anchor);
        std::string err;
        if (!syncWorldBuffers(w, vw, anchor, &err)) return ev::throwError("VoxelWorld.buffers: " + err);
        ev::ObjectBuilder o;
        o.set("regions", brokernel::api::bufferValue(w->gpuRegions));
        o.set("nodes", brokernel::api::bufferValue(w->gpuNodes));
        o.set("children", brokernel::api::bufferValue(w->gpuChildren));
        o.set("bricks", brokernel::api::bufferValue(w->gpuBricks));
        o.set("attrs", brokernel::api::bufferValue(w->gpuAttrs));
        o.set("animClips", brokernel::api::bufferValue(w->gpuAnimClips));
        o.set("animFrames", brokernel::api::bufferValue(w->gpuAnimFrames));
        o.set("regionCount", static_cast<double>(w->regionCount));
        return o.get();
    });

    b.accessor("node", [](Value self, std::span<const Value>) -> Value {
        scene::VoxelWorldNode* vw = nullptr;
        scene::SceneGraph* sg = nullptr;
        worldOf(self, "VoxelWorld.node", &vw, &sg);
        void* raw = wrapNode(vw, sg);
        if (!raw) return ev::null();
        const Rooted wrapped(ev::wrapNative(raw, "__bro_native.scene.SceneNode"));
        const auto nativeRoot = ev::globalValue("__bro_native");
        if (nativeRoot.found && ev::isObject(nativeRoot.value)) {
            const Rooted sc(ev::getProperty(nativeRoot.value, "scene"));
            if (ev::isObject(sc)) {
                const Rooted canonFn(ev::getProperty(sc, "canonNode"));
                if (ev::isFunction(canonFn)) {
                    const Value argv[1] = {wrapped};
                    const ev::CallResult r = ev::call(canonFn, sc, argv);
                    if (!r.thrown) return r.value;
                }
            }
        }
        return wrapped;
    });
}

}  // namespace

void installVoxelDag(engine::Engine& engine) {
    const std::string src = scene::vk::voxelDagModuleSource();
    brokernel::api::registerHostModule("voxeldag", src);
    g_worldClass.install("VoxelWorld", 0, nullptr, decorateWorldClass, /*global=*/false);
    g_jobClass.install("VoxelizeJob", 0, nullptr, decorateJobClass, /*global=*/false);
    static bool s_workerHook = false;
    if (!s_workerHook) {
        s_workerHook = true;
        engine.addShutdownHook([] { voxelizeWorker().stop(); });
    }

    const auto bro = ev::globalValue("bro");
    if (!bro.found || !ev::isObject(bro.value)) return;
    const Rooted broObj(bro.value);
    ev::ObjectBuilder vd;
    vd.def("createWorld", 1, js_createWorld);
    vd.set("module", src);
    ev::setProperty(broObj, "voxeldag", vd.get());
}

}  // namespace bro::bronze_host
