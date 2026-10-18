// native_vision.cpp — `__bro_native.vision`: the compute js/vision_wildlife.js
// hands to C++ (natives/vision/native_vision_decl.h). Plain arithmetic over
// typed arrays, no model: registered in every build, on the main thread and
// on each Worker's.
//
// dotRows is a crop's cosine similarity against every species prompt (the
// embeddings are L2-normalised, so a dot product). In JavaScript that was
// ~11 ms a crop against ~2000 prompts at 768 dims; here it is well under a
// millisecond, and the numbers are the JavaScript loop's bit for bit: each
// row's sum runs in float64 from i = 0 up, one product at a time. A float
// times a float is exact in a double, so a fused multiply-add (where the
// compiler contracts one) rounds the same as the separate add. The speed is
// from summing kRowBlock rows side by side (independent chains, one load of
// the query element for all of them) and from keeping a block of rows in
// cache across every query, never from reordering a sum.

#include "natives/vision/native_vision_decl.h"

#include "bronze_host/host_natives.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

constexpr uint32_t kRowBlock = 8;

void releaseMalloc(void* ctx) { std::free(ctx); }

void empty(bronze_native_buffer* out) {
    out->data = nullptr;
    out->length = 0;
    out->release = nullptr;
    out->ctx = nullptr;
}

}  // namespace

extern "C" {

void bro_vision_dotRows(const float* queries, uint32_t queries_len,
                        const float* rows, uint32_t rows_len,
                        int32_t dim, bronze_native_buffer* out) {
    if (dim < 1 || queries_len % uint32_t(dim) || rows_len % uint32_t(dim) ||
        !queries_len || !rows_len) {
        empty(out);
        return;
    }
    const uint32_t d = uint32_t(dim);
    const uint32_t nq = queries_len / d, nr = rows_len / d;
    const uint64_t count = uint64_t(nq) * nr;
    if (count > UINT32_MAX) { empty(out); return; }
    auto* res = static_cast<float*>(std::malloc(size_t(count) * sizeof(float)));
    if (!res) { empty(out); return; }

    uint32_t r0 = 0;
    for (; r0 + kRowBlock <= nr; r0 += kRowBlock) {
        const float* blk = rows + size_t(r0) * d;
        for (uint32_t q = 0; q < nq; q++) {
            const float* qv = queries + size_t(q) * d;
            double acc[kRowBlock] = {};
            for (uint32_t i = 0; i < d; i++) {
                const double e = qv[i];
                for (uint32_t k = 0; k < kRowBlock; k++) acc[k] += e * double(blk[size_t(k) * d + i]);
            }
            float* o = res + size_t(q) * nr + r0;
            for (uint32_t k = 0; k < kRowBlock; k++) o[k] = float(acc[k]);
        }
    }
    for (; r0 < nr; r0++) {
        const float* row = rows + size_t(r0) * d;
        for (uint32_t q = 0; q < nq; q++) {
            const float* qv = queries + size_t(q) * d;
            double acc = 0;
            for (uint32_t i = 0; i < d; i++) acc += double(qv[i]) * double(row[i]);
            res[size_t(q) * nr + r0] = float(acc);
        }
    }
    out->data = res;
    out->length = uint32_t(count);
    out->release = releaseMalloc;
    out->ctx = res;
}

}  // extern "C"

namespace bro::bronze_host {
}  // namespace bro::bronze_host
