// scene.windAt(): wind.bk's wind_velocity on the CPU, through brokernel's
// CPU build of the same source the scene shaders compile for the GPU.

#include "scene/world/wind_field.h"

#include "util/log.h"

#include <brokernel/brokernel.h>

#include <memory>
#include <mutex>

#include "wind.bk.src.h"

namespace bro::scene {

namespace {

constexpr const char* kWindSampleSource = R"(
import wind;

@kernel @cpu fn wind_sample(x: f32, z: f32, t: f32, dir_x: f32, dir_z: f32, base: f32, gust_speed: f32,
                            gust: f32, inv_along: f32, inv_across: f32) -> vec4<f32> {
    return wind::wind_velocity(vec2<f32>(x, z), t, vec2<f32>(dir_x, dir_z), base, gust_speed, gust,
                               vec2<f32>(inv_along, inv_across));
}
)";

// The compiled kernel, made once (null when it would not build).
const brokernel::CompiledKernel* windKernel() {
    static std::once_flag s_once;
    static std::unique_ptr<brokernel::CompileResult> s_result;
    static const brokernel::CompiledKernel* s_kernel = nullptr;
    std::call_once(s_once, [] {
        brokernel::Compiler compiler;
        compiler.register_module("wind", kBkWindSrc);
        brokernel::CompileOptions opts;
        opts.entry_name = "wind_sample";
        opts.emit_spirv = false;
        s_result = std::make_unique<brokernel::CompileResult>(
            compiler.compile_source(kWindSampleSource, "wind_sample.bk", opts));
        const brokernel::CompiledKernel* k = s_result->ok ? s_result->find_kernel("wind_sample") : nullptr;
        if (!k || !k->has_cpu_build()) {
            LOG_ERROR("scene.windAt: the wind kernel did not build: %s",
                      !s_result->ok ? s_result->error_message().c_str()
                      : k            ? k->cpu_error().c_str()
                                     : "no wind_sample kernel");
            return;
        }
        s_kernel = k;
    });
    return s_kernel;
}

}  // namespace

WindSample windAt(const WindFieldState& s, float x, float z) {
    WindSample w{};
    const brokernel::CompiledKernel* k = windKernel();
    if (!k) return w;
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::string err;
    if (!k->eval_cpu({brokernel::KernelArg(x), brokernel::KernelArg(z), brokernel::KernelArg(s.time),
                      brokernel::KernelArg(s.dirX), brokernel::KernelArg(s.dirZ), brokernel::KernelArg(s.base),
                      brokernel::KernelArg(s.gustSpeed), brokernel::KernelArg(s.gust),
                      brokernel::KernelArg(s.invAlong), brokernel::KernelArg(s.invAcross)},
                     v, &err)) {
        LOG_ERROR("scene.windAt: %s", err.c_str());
        return w;
    }
    w.velocity[0] = v[0];
    w.velocity[1] = v[1];
    w.velocity[2] = v[2];
    w.gust = v[3];
    w.speed = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    return w;
}

}  // namespace bro::scene
