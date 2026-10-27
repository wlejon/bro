// Place natives: the body a scene stands on and its place frame
// (scene/world/place.h), behind js/scene_graph.js's setBody, place, toBody,
// toPlace, upAt, latLongAt, reanchorAt, the `reanchor` event and expect.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_runtime.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/world/place.h"

#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <vector>

using namespace bro::bronze_host;

namespace {

bool point(const double* p, uint32_t n, double out[3]) {
    if (!p || n < 3 || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    return true;
}

}  // namespace

void bro_scene_SceneGraph_setBody(void* self, double radius, const double* spin, uint32_t spin_len, const double* at,
                                  uint32_t at_len) {
    auto* g = graphOf(self);
    if (!g) return;
    scene::Body body;
    if (radius > 0.0) body.radius = radius;   // (Infinity, or none: flat)
    double s[3];
    if (point(spin, spin_len, s)) std::copy(s, s + 3, body.spin);
    double p[3] = {0.0, 0.0, 0.0};
    point(at, at_len, p);
    g->setBody(body, p);
}

// place() -> [hasBody, radius (0: flat), spin x, y, z, face, anchor x, z,
// origin x, y, z, basis (9, row-major: column j axis j), serial, faceCells,
// lattice]
void bro_scene_SceneGraph_place(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g) { transferBuffer(std::vector<double>{}, out); return; }
    const scene::Place& p = g->place();
    std::vector<double> v;
    v.reserve(23);
    v.push_back(g->hasBody() ? 1.0 : 0.0);
    v.push_back(p.body().flat() ? 0.0 : p.body().radius);
    v.insert(v.end(), p.body().spin, p.body().spin + 3);
    v.push_back(p.anchor().face);
    v.push_back(p.anchor().x);
    v.push_back(p.anchor().z);
    v.insert(v.end(), p.origin(), p.origin() + 3);
    v.insert(v.end(), p.basis(), p.basis() + 9);
    v.push_back(static_cast<double>(p.serial()));
    v.push_back(p.faceCells());
    v.push_back(scene::Place::kLattice);
    transferBuffer(std::move(v), out);
}

void bro_scene_SceneGraph_toBody(void* self, const double* p, uint32_t p_len, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    double in[3], b[3];
    if (!g || !point(p, p_len, in)) { transferBuffer(std::vector<double>{}, out); return; }
    g->place().toBody(in, b);
    transferBuffer(std::vector<double>{b[0], b[1], b[2]}, out);
}

void bro_scene_SceneGraph_toPlace(void* self, const double* b, uint32_t b_len, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    double in[3], p[3];
    if (!g || !point(b, b_len, in)) { transferBuffer(std::vector<double>{}, out); return; }
    g->place().toPlace(in, p);
    transferBuffer(std::vector<double>{p[0], p[1], p[2]}, out);
}

void bro_scene_SceneGraph_upAt(void* self, const double* p, uint32_t p_len, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    double in[3];
    if (!g || !point(p, p_len, in)) { transferBuffer(std::vector<double>{}, out); return; }
    const bromath::Vec3 u = g->place().up({static_cast<float>(in[0]), static_cast<float>(in[1]), static_cast<float>(in[2])});
    transferBuffer(std::vector<double>{u.x, u.y, u.z}, out);
}

void bro_scene_SceneGraph_latLongAt(void* self, const double* p, uint32_t p_len, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    double in[3];
    if (!g || !point(p, p_len, in)) { transferBuffer(std::vector<double>{}, out); return; }
    double lat = 0.0, lon = 0.0;
    g->place().latLong({static_cast<float>(in[0]), static_cast<float>(in[1]), static_cast<float>(in[2])}, lat, lon);
    transferBuffer(std::vector<double>{lat, lon}, out);
}

bool bro_scene_SceneGraph_reanchorAt(void* self, const double* b, uint32_t b_len) {
    auto* g = graphOf(self);
    double in[3];
    return g && point(b, b_len, in) && g->reanchorAt(in);
}

// Where the eye is expected (scene/world/expected_eyes.h): an id, 0 when
// ExpectedEyes::kMax are held. `within` is seconds (Infinity: unsaid).
double bro_scene_SceneGraph_expect(void* self, const double* b, uint32_t b_len, double within) {
    auto* g = graphOf(self);
    double in[3];
    if (!g || !point(b, b_len, in)) return 0.0;
    return g->expected().add(in, std::isnan(within) ? std::numeric_limits<double>::infinity() : within);
}

bool bro_scene_SceneGraph_expectMove(void* self, double id, const double* b, uint32_t b_len, double within) {
    auto* g = graphOf(self);
    double in[3];
    if (!g || !point(b, b_len, in)) return false;
    return g->expected().move(static_cast<uint32_t>(id), in,
                              std::isnan(within) ? std::numeric_limits<double>::infinity() : within);
}

void bro_scene_SceneGraph_expectCancel(void* self, double id) {
    if (auto* g = graphOf(self)) g->expected().cancel(static_cast<uint32_t>(id));
}

// What is still to be made there; -1 until the world has looked.
double bro_scene_SceneGraph_expectPending(void* self, double id) {
    auto* g = graphOf(self);
    return g ? g->expected().pending(static_cast<uint32_t>(id)) : -1.0;
}

// The handler is called with a Float64Array of 12: the move's rotation
// (row-major) then its translation, new = rot * old + t.
void bro_scene_SceneGraph_onReanchor_set(void* self, uint64_t cbBits) {
    auto* g = graphOf(self);
    if (!g) return;
    Value cb = ev::fromBits(cbBits);
    if (!ev::isFunction(cb)) {
        g->setOnReanchor(nullptr);
        return;
    }
    auto fn = std::make_shared<ev::Persistent>(cb);
    g->setOnReanchor([fn](const scene::PlaceMove& m) {
        double words[12];
        std::copy(m.rot, m.rot + 9, words);
        std::copy(m.t, m.t + 3, words + 9);
        ev::Persistent arr(ev::createTypedArray(ev::elements::Float64, 12));
        ev::fillTypedArray(arr.get(), std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(words), sizeof(words)));
        Value args[1] = {arr.get()};
        ev::CallResult r = ev::call(fn->get(), ev::undefined(), args);
        if (r.thrown) reportBronzeError("scene reanchor", r.value);
    });
}
