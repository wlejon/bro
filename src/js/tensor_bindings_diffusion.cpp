#if BRO_WITH_TENSOR
// JS bindings — diffusion sampler steps and sinusoidal timestep embedding.
// See tensor_bindings.cpp for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ddimStep(x_t, eps_pred, alpha_t, alpha_prev, sigma_t, x_prev)
static JSValue js_ddimStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "ddimStep(x_t,eps_pred,alpha_t,alpha_prev,sigma_t,x_prev)");
    ENSURE_INIT();
    GT(xt,  0, "ddimStep"); GT(eps, 1, "ddimStep");
    double at = 0, ap = 0, st = 0;
    JS_ToFloat64(ctx, &at, argv[2]);
    JS_ToFloat64(ctx, &ap, argv[3]);
    JS_ToFloat64(ctx, &st, argv[4]);
    GT(xp, 5, "ddimStep");
    nngpu::ddim_step(*xt, *eps, (float)at, (float)ap, (float)st, *xp);
    return JS_UNDEFINED;
}

// eulerStep(x_t, eps_pred, sigma_t, sigma_prev, x_prev)
static JSValue js_eulerStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "eulerStep(x_t,eps_pred,sigma_t,sigma_prev,x_prev)");
    ENSURE_INIT();
    GT(xt,  0, "eulerStep"); GT(eps, 1, "eulerStep");
    double st = 0, sp = 0;
    JS_ToFloat64(ctx, &st, argv[2]);
    JS_ToFloat64(ctx, &sp, argv[3]);
    GT(xp, 4, "eulerStep");
    nngpu::euler_step(*xt, *eps, (float)st, (float)sp, *xp);
    return JS_UNDEFINED;
}

// dpmpp2mStep(x_t, eps_pred, x0_prev, sigma_t, c_xt, c_x0t, c_x0prev,
//             x_prev, x0_out)
static JSValue js_dpmpp2mStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "dpmpp2mStep(x_t,eps_pred,x0_prev,sigma_t,c_xt,c_x0t,c_x0prev,x_prev,x0_out)");
    ENSURE_INIT();
    GT(xt,  0, "dpmpp2mStep"); GT(eps,    1, "dpmpp2mStep");
    GT(x0p, 2, "dpmpp2mStep");
    double st = 0, c_xt = 0, c_x0t = 0, c_x0prev = 0;
    JS_ToFloat64(ctx, &st,       argv[3]);
    JS_ToFloat64(ctx, &c_xt,     argv[4]);
    JS_ToFloat64(ctx, &c_x0t,    argv[5]);
    JS_ToFloat64(ctx, &c_x0prev, argv[6]);
    GT(xp, 7, "dpmpp2mStep"); GT(x0o, 8, "dpmpp2mStep");
    nngpu::dpmpp_2m_step(*xt, *eps, *x0p, (float)st,
                             (float)c_xt, (float)c_x0t, (float)c_x0prev,
                             *xp, *x0o);
    return JS_UNDEFINED;
}

// timestepEmbedding(timesteps, dim, maxPeriod, Y)
static JSValue js_timestepEmbedding(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "timestepEmbedding(timesteps,dim,maxPeriod,Y)");
    ENSURE_INIT();
    GT(T, 0, "timestepEmbedding");
    int32_t dim = 0;
    JS_ToInt32(ctx, &dim, argv[1]);
    double maxPeriod = 10000.0; JS_ToFloat64(ctx, &maxPeriod, argv[2]);
    GT(Y, 3, "timestepEmbedding");
    nngpu::timestep_embedding(*T, dim, (float)maxPeriod, *Y);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorDiffusionOps(JSContext* ctx, JSValue gpuObj) {
    JS_SetPropertyStr(ctx, gpuObj, "ddimStep",          JS_NewCFunction(ctx, js_ddimStep,          "ddimStep",          6));
    JS_SetPropertyStr(ctx, gpuObj, "eulerStep",         JS_NewCFunction(ctx, js_eulerStep,         "eulerStep",         5));
    JS_SetPropertyStr(ctx, gpuObj, "dpmpp2mStep",       JS_NewCFunction(ctx, js_dpmpp2mStep,       "dpmpp2mStep",       9));
    JS_SetPropertyStr(ctx, gpuObj, "timestepEmbedding", JS_NewCFunction(ctx, js_timestepEmbedding, "timestepEmbedding", 4));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorDiffusionOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU

#endif  // BRO_WITH_TENSOR
