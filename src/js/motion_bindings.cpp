#if BRO_WITH_DIFFUSION && BRO_WITH_LM

#include "js/motion_bindings.h"
#include "util/interrupt.h"
#include <qjsbind/qjsbind.h>
#include <brolm/llama3_tokenizer.h>
#include <brolm/llm2vec.h>
#include <brodiffusion/ardy/denoiser.h>
#include <brodiffusion/ardy/fsq_decoder.h>
#include <brodiffusion/ardy/sampler.h>
#include <brodiffusion/ardy/motion_rep.h>
#include <brodiffusion/ardy/text_conditioner.h>
#include <brotensor/runtime.h>
#include <brotensor/safetensors.h>
#include <brotensor/tensor.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace bt   = ::brotensor;
namespace st   = ::brotensor::safetensors;
namespace ardy = ::brodiffusion::ardy;

namespace {

bool mtGetStr(JSContext* ctx, JSValueConst obj, const char* key, std::string& out) {
    if (!JS_IsObject(obj)) return false;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

void mtGetInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; }
    JS_FreeValue(ctx, v);
}

void mtGetFloat(JSContext* ctx, JSValueConst obj, const char* key, float& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
    JS_FreeValue(ctx, v);
}

bt::Device mtAutoDevice() {
    if (bt::is_available(bt::Device::CUDA))  return bt::Device::CUDA;
    if (bt::is_available(bt::Device::Metal)) return bt::Device::Metal;
    return bt::Device::CPU;
}

std::vector<float> loadNpyF32(const std::string& path, int n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    char magic[8]; in.read(magic, 8);
    unsigned char hl[2]; in.read(reinterpret_cast<char*>(hl), 2);
    in.seekg(10 + (hl[0] | (hl[1] << 8)), std::ios::beg);
    std::vector<float> v(n);
    in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(float)));
    if (!in) throw std::runtime_error("short read from " + path);
    return v;
}
std::vector<float> loadNpyF64AsF32(const std::string& path, int n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    char magic[8]; in.read(magic, 8);
    unsigned char hl[2]; in.read(reinterpret_cast<char*>(hl), 2);
    in.seekg(10 + (hl[0] | (hl[1] << 8)), std::ios::beg);
    std::vector<double> d(n);
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(n * sizeof(double)));
    if (!in) throw std::runtime_error("short read from " + path);
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i) v[i] = static_cast<float>(d[i]);
    return v;
}

JSValue mtFloat32Array(JSContext* ctx, const float* d, std::size_t n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t*>(d), n * sizeof(float));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return arr;
}

JSValue mtInt32Array(JSContext* ctx, const std::int32_t* d, std::size_t n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t*>(d), n * sizeof(std::int32_t));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Int32Array");
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return arr;
}

struct ArdyMotionWrapper {
    std::unique_ptr<brolm::llama3::Tokenizer>   tok;
    std::unique_ptr<brolm::llm2vec::Encoder>    enc;
    std::unique_ptr<ardy::ArdyDenoiser>         denoiser;
    std::unique_ptr<ardy::FsqMotionDecoder>     fsq;
    std::unique_ptr<ardy::ArdyMotionGenerator>  gen;
    ardy::ArdyMotionRep                         rep{25.0};
    bt::Device device = bt::Device::CPU;
};

JSValue mtGenerate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<ArdyMotionWrapper>(ctx, this_val);
    if (!w || !w->gen || !w->enc || !w->tok)
        return JS_ThrowTypeError(ctx, "motion: pipeline not loaded");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "generate(text, opts): text (string) required");

    const char* cstr = JS_ToCString(ctx, argv[0]);
    std::string text = cstr ? cstr : "";
    if (cstr) JS_FreeCString(ctx, cstr);

    int   frames = 104, steps = 10, seed = 0;
    float cfg = 2.5f, heading = 0.0f;
    if (argc > 1 && JS_IsObject(argv[1])) {
        mtGetInt(ctx, argv[1], "frames", frames);
        mtGetInt(ctx, argv[1], "steps", steps);
        mtGetInt(ctx, argv[1], "seed", seed);
        mtGetFloat(ctx, argv[1], "cfg", cfg);
        mtGetFloat(ctx, argv[1], "heading", heading);
    }
    if (frames < 1) frames = 1;

    try {
        std::vector<float> feat;
        ardy::ardy_text_feat(*w->tok, *w->enc, text, feat);
        if (bro::util::interrupted()) return JS_ThrowInternalError(ctx, "motion.generate interrupted");

        const int hyb = w->denoiser->hybrid_dim();
        const int fpt = w->denoiser->config().num_frames_per_token;
        const int G   = 52 / fpt;
        const int W   = w->gen->num_windows(frames);
        std::mt19937_64 rng(static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)));
        std::normal_distribution<float> norm(0.0f, 1.0f);
        std::vector<float> noise(static_cast<std::size_t>(W) * G * hyb);
        for (float& v : noise) v = norm(rng);

        std::vector<float> hybrid;
        int T_tok = 0;
        w->gen->generate_hybrid(feat.data(), frames, heading, steps, cfg,
                                noise.data(), hybrid, T_tok);
        if (bro::util::interrupted()) return JS_ThrowInternalError(ctx, "motion.generate interrupted");

        std::vector<float> motion;
        w->gen->detokenize_to_motion(hybrid.data(), T_tok, motion);
        const int mrd = w->denoiser->config().motion_rep_dim;
        const int F   = T_tok * fpt;

        std::vector<double> mdbl(motion.begin(), motion.end());
        ardy::ArdyMotionRep::Decoded dec =
            w->rep.inverse(mdbl.data(), F, /*is_normalized=*/true);
        const int J = ardy::ArdyMotionRep::kNumJoints;

        std::vector<float> pos(static_cast<std::size_t>(F) * J * 3);
        for (std::size_t i = 0; i < pos.size(); ++i)
            pos[i] = static_cast<float>(dec.posed_joints[i]);
        std::vector<float> contacts(static_cast<std::size_t>(F) * 4);
        for (std::size_t i = 0; i < contacts.size() && i < dec.foot_contacts.size(); ++i)
            contacts[i] = static_cast<float>(dec.foot_contacts[i]);

        std::vector<std::int32_t> parents(J);
        const int* jp = ardy::G1Skeleton::joint_parents();
        for (int j = 0; j < J; ++j) parents[j] = jp[j];

        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "frames",      JS_NewInt32(ctx, F));
        JS_SetPropertyStr(ctx, out, "joints",      JS_NewInt32(ctx, J));
        JS_SetPropertyStr(ctx, out, "fps",         JS_NewFloat64(ctx, w->rep.fps()));
        JS_SetPropertyStr(ctx, out, "positions",   mtFloat32Array(ctx, pos.data(), pos.size()));
        JS_SetPropertyStr(ctx, out, "parents",     mtInt32Array(ctx, parents.data(), parents.size()));
        JS_SetPropertyStr(ctx, out, "footContacts", mtFloat32Array(ctx, contacts.data(), contacts.size()));
        (void)mrd;
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "motion.generate failed: %s", e.what());
    }
}

void mtRegisterClass(JSContext* ctx) {
    qjsbind::Class<ArdyMotionWrapper>(ctx, "ArdyMotionPipeline", qjsbind::NoGlobal)
        .get("device", [](ArdyMotionWrapper* w) {
            switch (w->device.type) {
                case bt::DeviceType::CUDA:  return std::string("CUDA");
                case bt::DeviceType::Metal: return std::string("Metal");
                default:                    return std::string("CPU");
            }
        })
        .method_raw("generate", mtGenerate, 2);
}

JSValue mtLoad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "load({ checkpoint, textEncoder }) requires an options object");

    std::string ckpt, tenc, dev;
    if (!mtGetStr(ctx, argv[0], "checkpoint", ckpt) || !mtGetStr(ctx, argv[0], "textEncoder", tenc))
        return JS_ThrowTypeError(ctx, "load: checkpoint and textEncoder paths are required");

    try {
        bt::init();
        bt::Device device = mtAutoDevice();
        if (mtGetStr(ctx, argv[0], "device", dev)) {
            if (dev == "cpu") device = bt::Device::CPU;
            else if (dev == "cuda") device = bt::Device::CUDA;
            else if (dev == "metal") device = bt::Device::Metal;
        }
        bt::set_default_device(device);

        auto w = std::make_unique<ArdyMotionWrapper>();
        w->device = device;

        w->denoiser = std::make_unique<ardy::ArdyDenoiser>();
        { st::File f = st::File::open(ckpt + "/denoiser.safetensors"); w->denoiser->load_weights(f); }
        const int sdim = w->denoiser->stats_dim();
        auto mean = loadNpyF64AsF32(ckpt + "/stats/motion/mean.npy", sdim);
        auto std_ = loadNpyF64AsF32(ckpt + "/stats/motion/std.npy",  sdim);
        w->denoiser->set_motion_stats(mean.data(), std_.data(), sdim);
        w->rep.set_motion_stats(mean.data(), std_.data(), sdim);

        w->fsq = std::make_unique<ardy::FsqMotionDecoder>();
        { st::File f = st::File::open(ckpt + "/tokenizer.safetensors"); w->fsq->load_weights(f); }
        const int td = w->fsq->config().token_dim;
        auto qmean = loadNpyF32(ckpt + "/stats/post_quantization/mean.npy", td);
        auto qstd  = loadNpyF32(ckpt + "/stats/post_quantization/std.npy",  td);
        w->fsq->set_post_quant_stats(qmean.data(), qstd.data(), td);

        w->gen = std::make_unique<ardy::ArdyMotionGenerator>(*w->denoiser, *w->fsq);

        w->tok = std::make_unique<brolm::llama3::Tokenizer>(
            brolm::llama3::Tokenizer::load(tenc + "/tokenizer.json"));
        brolm::llm2vec::Config lcfg = brolm::llm2vec::Config::load(tenc + "/config.json");
        w->enc = std::make_unique<brolm::llm2vec::Encoder>(lcfg);
        { st::File f = st::File::open(tenc + "/model.safetensors"); w->enc->load_weights(f); }

        return qjsbind::wrap<ArdyMotionWrapper>(ctx, w.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "motion.load failed: %s", e.what());
    }
}

JSValue mtInit(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try { bt::init(); } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "motion.init failed: %s", e.what());
    }
    return JS_UNDEFINED;
}
}


// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installMotionBindings(JSContext* ctx) {
    mtRegisterClass(ctx);
    
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }
    
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "init", JS_NewCFunction(ctx, mtInit, "init", 0));
    JS_SetPropertyStr(ctx, ns, "load", JS_NewCFunction(ctx, mtLoad, "load", 1));
    JS_SetPropertyStr(ctx, broObj, "motion", ns);
    
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js

#endif // BRO_WITH_DIFFUSION && BRO_WITH_LM
