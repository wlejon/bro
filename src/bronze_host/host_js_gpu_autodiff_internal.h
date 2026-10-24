#pragma once

#include "bronze_host/host_js_gpu.h"
#include "codegen-brass/js_gpu_autodiff.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

struct CompiledJsAutodiffEntry {
    bronze::GpuAutodiffResult compiled;
};

std::shared_ptr<CompiledJsAutodiffEntry> lookupOrCompileAutodiffEntry(
    Value arg,
    bool defaultSdfMode,
    std::string* outRawSource);

bronze::GpuAutodiffSample evalAutodiffAnyArity(
    const bronze::GpuAutodiffResult& compiled,
    const std::vector<float>& scalarArgs,
    bool forceSpirv = false);

Value buildGradSampleObject(
    const bronze::GpuAutodiffSample& s,
    const bronze::GpuAutodiffResult& compiled);

Value hostAutodiffNumberArray(const double* vals, size_t count);

}  // namespace bro::bronze_host
