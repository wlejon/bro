// native_tensor.cpp — Bronze native entry points for bro.tensor
#include "natives/tensor/native_tensor_decl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BroGpuTensorImpl {
    int32_t rows = 0;
    int32_t cols = 0;
    std::string dtype = "fp32";
    std::vector<uint8_t> data;
};

}  // namespace

extern "C" {

void bro_tensor_GpuTensor_dtor(void* self) {
    delete static_cast<BroGpuTensorImpl*>(self);
}

void* bro_tensor_GpuTensor_ctor(void) {
    return new BroGpuTensorImpl();
}

int32_t bro_tensor_GpuTensor_rows_get(void* self) {
    return self ? static_cast<BroGpuTensorImpl*>(self)->rows : 0;
}

int32_t bro_tensor_GpuTensor_cols_get(void* self) {
    return self ? static_cast<BroGpuTensorImpl*>(self)->cols : 0;
}

int32_t bro_tensor_GpuTensor_size_get(void* self) {
    if (!self) return 0;
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    return t->rows * t->cols;
}

int32_t bro_tensor_GpuTensor_bytes_get(void* self) {
    if (!self) return 0;
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    return static_cast<int32_t>(t->data.size());
}

void bro_tensor_GpuTensor_zero(void* self) {
    if (!self) return;
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    std::fill(t->data.begin(), t->data.end(), 0);
}

const char* bro_tensor_GpuTensor_dtype(void* self) {
    return self ? static_cast<BroGpuTensorImpl*>(self)->dtype.c_str() : "fp32";
}

void* bro_tensor_GpuTensor_clone(void* self) {
    if (!self) return new BroGpuTensorImpl();
    return new BroGpuTensorImpl(*static_cast<BroGpuTensorImpl*>(self));
}

void bro_tensor_GpuTensor_uploadFp16(void* self, const uint16_t* data, uint32_t data_len) {
    if (!self || !data) return;
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    t->data.resize(data_len * sizeof(uint16_t));
    std::memcpy(t->data.data(), data, data_len * sizeof(uint16_t));
}

void bro_tensor_GpuTensor_downloadFp16(void* self, bronze_native_buffer* out) {
    if (!out) return;
    if (!self) { out->data = nullptr; out->length = 0; out->release = nullptr; return; }
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    out->data = t->data.data();
    out->length = t->data.size() / sizeof(uint16_t);
    out->release = nullptr;
}

void bro_tensor_GpuTensor_uploadInt8(void* self, const int8_t* data, uint32_t data_len) {
    if (!self || !data) return;
    auto* t = static_cast<BroGpuTensorImpl*>(self);
    t->data.resize(data_len);
    std::memcpy(t->data.data(), data, data_len);
}

#if BRO_WITH_TENSOR
bool bro_tensor_available_get(void) { return true; }
const char* bro_tensor_backend_get(void) { return "cpu"; }
void bro_tensor_init(void) {}
void bro_tensor_sync(void) {}
void bro_tensor_linearForward(void*, void*, void*, void*) {}
void bro_tensor_linearBackward(void*, void*, void*, void*, void*, void*) {}
void bro_tensor_reluForward(void*, void*) {}
void bro_tensor_reluBackward(void*, void*, void*) {}
void bro_tensor_tanhForward(void*, void*) {}
void bro_tensor_tanhBackward(void*, void*, void*) {}
void bro_tensor_sigmoidForward(void*, void*) {}
void bro_tensor_sigmoidBackward(void*, void*, void*) {}
void bro_tensor_addInplace(void*, void*) {}
void bro_tensor_addScalarInplace(void*, double) {}
void bro_tensor_scaleInplace(void*, double) {}
void bro_tensor_mulInplace(void*, void*) {}
void bro_tensor_clamp(void*, double, double) {}
void bro_tensor_buildSlotMask(void*, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_copyD2D(void*, int32_t, void*, int32_t, int32_t) {}
void bro_tensor_siluForward(void*, void*) {}
void bro_tensor_siluBackward(void*, void*, void*) {}
void bro_tensor_geluForward(void*, void*) {}
void bro_tensor_geluBackward(void*, void*, void*) {}
void bro_tensor_geluExactForward(void*, void*) {}
void bro_tensor_geluExactBackward(void*, void*, void*) {}
void bro_tensor_quickGeluForward(void*, void*) {}
void bro_tensor_quickGeluBackward(void*, void*, void*) {}
void bro_tensor_swigluForward(void*, void*) {}
void bro_tensor_swigluBackward(void*, void*, void*) {}
void bro_tensor_gegluForward(void*, void*) {}
void bro_tensor_gegluBackward(void*, void*, void*) {}
void bro_tensor_gegluExactForward(void*, void*) {}
void bro_tensor_gegluExactBackward(void*, void*, void*) {}
void bro_tensor_softmaxBackward(void*, void*, void*) {}
void bro_tensor_layernormBackward(void*, void*, void*, double, void*, void*, void*) {}
void bro_tensor_layernormForwardInferenceBatched(void*, void*, void*, void*, double) {}
void bro_tensor_layernormForwardInferenceBatchedFp16(void*, void*, void*, void*, double) {}
void bro_tensor_rmsNormForward(void*, void*, double, void*) {}
void bro_tensor_rmsNormBackward(void*, void*, void*, double, void*, void*) {}
void bro_tensor_groupNormForward(void*, void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, double, void*) {}
void bro_tensor_groupNormBackward(void*, void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, double, void*, void*, void*) {}
void bro_tensor_matmul(void*, void*, void*) {}
void bro_tensor_matmulBackward(void*, void*, void*, void*, void*) {}
void bro_tensor_ropeForward(void*, int32_t, int32_t, int32_t, double, void*) {}
void bro_tensor_ropeBackward(void*, int32_t, int32_t, int32_t, double, void*) {}
void bro_tensor_ropeApply(void*, void*, void*, int32_t, int32_t, void*) {}
void bro_tensor_ropeApplyBackward(void*, int32_t, int32_t, void*) {}
void bro_tensor_modulate(void*, void*, void*, void*) {}
void bro_tensor_broadcastMul(void*, void*, void*) {}
void bro_tensor_sumRows(void*, void*) {}
void bro_tensor_sumCols(void*, void*) {}
void bro_tensor_argmaxRows(void*, void*) {}
void bro_tensor_attentionTokenMoments(void*, int32_t, int32_t, void*, void*) {}
void bro_tensor_buildCausalMaskRow(int32_t, int32_t, void*) {}
void bro_tensor_flashAttentionDecode(void*, void*, void*, int32_t, int32_t, void*, bool, int32_t, double, int32_t) {}
void bro_tensor_flashAttentionDecodeMasked(void*, void*, void*, void*, int32_t, void*, bool, int32_t, double, int32_t) {}
void bro_tensor_kvCacheAppend(void*, void*, int32_t, void*, void*) {}
void bro_tensor_conv2dBackwardInput(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_conv2dBackwardWeight(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_conv2dBackwardBias(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_upsampleNearest2xForward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_upsampleNearest2xBackward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_upsampleBilinear2xForward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_upsampleBilinear2xBackward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_downsampleAvg2xForward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_downsampleAvg2xBackward(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_nchwToSequence(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_sequenceToNchw(void*, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_interp2dForward(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_interp2dAlignCornersForward(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_unfold2dForward(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
void bro_tensor_l2NormalizeNchwForward(void*, int32_t, int32_t, int32_t, int32_t, double, void*) {}
void bro_tensor_convexUpsampleForward(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}
double bro_tensor_mseVecForward(void*, void*) { return 0.0; }
void bro_tensor_mseVecBackward(void*, void*, void*) {}
void bro_tensor_mseVecPerSample(void*, void*, void*, void*) {}
void bro_tensor_embeddingLookupForward(void*, void*, int32_t, void*) {}
void bro_tensor_embeddingLookupBackward(void*, void*, int32_t, void*) {}
void bro_tensor_sgdStep(void*, void*, void*, double, double) {}
void bro_tensor_adamStep(void*, void*, void*, void*, double, double, double, double, int32_t) {}
#endif

}  // extern "C"

namespace bro::bronze_host {
bool registerTensorNatives(std::string* error) {
    extern bool registerNatives_tensor(std::string* error);
    return registerNatives_tensor(error);
}
}
