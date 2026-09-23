// BiRefNet (background removal) and StyleGAN3 (generate / synthesize /
// invert) on bro's job machine.
//
// BiRefNet's result is the one place the old binding used TWO bitmaps:
// `matte` (the alpha plane as gray) and `image` (the source pixels with
// their alpha replaced — the ready-to-draw cutout). Both are back. The
// typed-array planes stay where the sibling put them: `alpha` (Float32),
// `mask` (the matte as bytes, the bronze port's name for it) and `data`
// (the cutout RGBA). Only `matte` changes type, from the byte plane to the
// ImageBitmap it was before the transition — `mask` is the same bytes.
//
// StyleGAN3 renders at 256²-1024², which is tens of milliseconds of GPU work
// per frame and several seconds for an inversion, so all three of its ops
// take `onDone`.

#if BRO_WITH_VISION

#include "bronze_host/host_vision_ops_internal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <utility>

namespace bro::bronze_host {

namespace {

namespace sg3 = brovisionml::stylegan3;

// ---------------------------------------------------------------------------
// BiRefNet.removeBackground(image, opts?) / .estimate(...)
//   -> { width, height, alpha, mask, matte, data, image }
// ---------------------------------------------------------------------------

struct BirefnetJob {
    bvm::BirefnetWrapper* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 0;
    int inH = 0;
    int modelSize = 1024;
    bool live = false;
    brovisionml::birefnet::Matte matte;
};

Value birefnetRemoveBackground(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::BirefnetWrapper>(bvm::g_birefnetClass, thisVal,
                                               bvm::kHostBirefnetTag);
    if (!w) {
        return ev::throwTypeError(
            "BiRefNet.prototype.removeBackground: not a BiRefNet instance");
    }
    if (args.empty()) {
        return ev::throwTypeError("removeBackground(image, opts?): image required");
    }

    auto job = std::make_shared<BirefnetJob>();
    job->w = w;
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err)) {
        return ev::throwTypeError(std::string("removeBackground: ") + err);
    }
    job->modelSize = w->modelSize;
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    visionIntOpt(optsRoot.get(), "modelSize", job->modelSize);
    job->live = w->loaded && w->net;

    BRO_VISION_BEGIN(w, "removeBackground")
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        job->matte.width = job->inW;
        job->matte.height = job->inH;
        if (!job->live) {
            job->matte.alpha.assign(static_cast<std::size_t>(job->inW) * job->inH, 0.0f);
            return;
        }
        // removeBackground() consumes interleaved float RGB; the decoded
        // alpha plane does not participate in matting.
        const std::size_t n = static_cast<std::size_t>(job->inW) * job->inH;
        std::vector<float> rgb(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            rgb[3 * i + 0] = job->rgba[4 * i + 0];
            rgb[3 * i + 1] = job->rgba[4 * i + 1];
            rgb[3 * i + 2] = job->rgba[4 * i + 2];
        }
        brotensor::DeviceScope scope(job->w->device);
        job->matte = job->w->net->removeBackground(rgb.data(), job->inW, job->inH,
                                                   /*rgbIs255=*/true, job->modelSize);
    };

    auto build = [job]() -> Value {
        const auto& m = job->matte;
        std::vector<uint8_t> bytes(m.alpha.size());
        for (std::size_t i = 0; i < m.alpha.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(
                std::lround(std::clamp(m.alpha[i], 0.0f, 1.0f) * 255.0f));
        }
        std::vector<uint8_t> cut = job->rgba;
        for (std::size_t i = 0; i < m.alpha.size() && i * 4 + 3 < cut.size(); ++i) {
            cut[4 * i + 3] = bytes[i];
        }

        ev::Persistent matteBmp(visionBitmapGrayUnit(m.alpha, m.width, m.height, false));
        ev::Persistent cutBmp(visionBitmapRGBA(cut.data(), m.width, m.height));

        ObjectBuilder res;
        res.set("width", ev::fromDouble(m.width));
        res.set("height", ev::fromDouble(m.height));
        {
            ev::Persistent a(bvm::makeFloat32Array(m.alpha.data(), m.alpha.size()));
            res.set("alpha", a.get());
        }
        {
            ev::Persistent b(bvm::makeUint8Array(bytes.data(), bytes.size()));
            res.set("mask", b.get());
        }
        {
            ev::Persistent c(bvm::makeUint8Array(cut.data(), cut.size()));
            res.set("data", c.get());
        }
        res.set("matte", matteBmp.get());
        res.set("image", cutBmp.get());
        return res.get();
    };

    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// StyleGAN3
// ---------------------------------------------------------------------------

// { data, width, height, channels, [seed], [w, numWs, wDim], [loss,
// lossCurve], image } — the sibling's sg3Result with the bitmap put back.
struct Sg3Out {
    sg3::Image img;
    int64_t seed = -1;
    std::vector<float> ws;
    bool hasLoss = false;
    float loss = 0.0f;
    std::vector<float> lossCurve;
};

Value sg3Result(const bvm::StyleGAN3Wrapper& w, const Sg3Out& out) {
    ev::Persistent bmp(visionBitmapRGB(out.img.rgb.data(), out.img.width, out.img.height));
    ObjectBuilder res;
    {
        ev::Persistent d(bvm::makeUint8Array(out.img.rgb.data(), out.img.rgb.size()));
        res.set("data", d.get());
    }
    res.set("width", ev::fromDouble(out.img.width));
    res.set("height", ev::fromDouble(out.img.height));
    res.set("channels", ev::fromDouble(out.img.channels));
    if (out.seed >= 0) res.set("seed", ev::fromDouble(static_cast<double>(out.seed)));
    if (!out.ws.empty()) {
        {
            ev::Persistent wv(bvm::makeFloat32Array(out.ws.data(), out.ws.size()));
            res.set("w", wv.get());
        }
        res.set("numWs", ev::fromDouble(w.numWs));
        res.set("wDim", ev::fromDouble(w.wDim));
    }
    if (out.hasLoss) res.set("loss", ev::fromDouble(out.loss));
    if (!out.lossCurve.empty()) {
        ev::Persistent lc(bvm::makeFloat32Array(out.lossCurve.data(), out.lossCurve.size()));
        res.set("lossCurve", lc.get());
    }
    res.set("image", bmp.get());
    return res.get();
}

// The sibling refuses an un-loaded generator up front, before reading any
// option (native_vision_generative.cpp); there is no placeholder image.
constexpr const char* kSg3NotLoaded =
    "StyleGAN3: generator is uninitialized or model weights not loaded";

struct Sg3Job {
    bvm::StyleGAN3Wrapper* w = nullptr;
    std::vector<float> z;
    std::vector<float> win;
    float psi = 1.0f;
    int cutoff = -1;
    bool returnLatents = false;
    // invert
    std::vector<uint8_t> rgba;
    int inW = 0;
    int inH = 0;
    int steps = 350;
    float lr = 0.05f;
    float regW = 0.0f;
    float initNoise = 0.0f;
    int64_t seed = 0;
    std::vector<float> initW;
    Sg3Out out;
};

Value sgGenerate(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::StyleGAN3Wrapper>(bvm::g_stylegan3Class, thisVal,
                                                bvm::kHostStyleGAN3Tag);
    if (!w) return ev::throwTypeError("StyleGAN3.prototype.generate: not a generator");
    if (!(w->loaded && w->generator)) return ev::throwError(kSg3NotLoaded);

    ev::Persistent optsRoot(args.empty() ? ev::undefined() : args[0]);
    auto job = std::make_shared<Sg3Job>();
    job->w = w;
    visionFloatOpt(optsRoot.get(), "truncation", job->psi);
    visionIntOpt(optsRoot.get(), "truncationCutoff", job->cutoff);
    job->returnLatents = visionBoolOpt(optsRoot.get(), "returnLatents", false);

    // An explicit z wins over a seed.
    bool gotZ = false;
    if (visionFloatsOpt(optsRoot.get(), "z", job->z)) {
        if (static_cast<int>(job->z.size()) == w->zDim) {
            gotZ = true;
        } else if (!job->z.empty()) {
            return ev::throwTypeError("generate: opts.z must have length zDim");
        }
    }
    job->seed = -1;
    if (!gotZ) {
        job->seed = 0;
        visionInt64Opt(optsRoot.get(), "seed", job->seed);
        job->z.resize(static_cast<std::size_t>(w->zDim));
        std::mt19937_64 rng(static_cast<unsigned long long>(job->seed));
        std::normal_distribution<float> nd(0.0f, 1.0f);
        for (float& v : job->z) v = nd(rng);
    }

    BRO_VISION_BEGIN(w, "generate")
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        auto* g = job->w;
        brotensor::DeviceScope scope(g->device);
        brotensor::Tensor zt = brotensor::Tensor::mat(1, g->zDim);
        for (int i = 0; i < g->zDim; ++i) zt[i] = job->z[static_cast<std::size_t>(i)];
        if (g->device != brotensor::Device::CPU) zt = zt.to(g->device);
        brotensor::Tensor ws = g->generator->map(zt, job->psi, job->cutoff);
        job->out.img = g->generator->render(ws);
        job->out.seed = job->seed;
        if (job->returnLatents) {
            brotensor::Tensor wc = ws.to(brotensor::Device::CPU);
            const float* p = wc.host_f32();
            job->out.ws.assign(p, p + static_cast<std::size_t>(g->numWs) * g->wDim);
        }
    };
    auto build = [job]() -> Value { return sg3Result(*job->w, job->out); };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

Value sgSynthesize(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::StyleGAN3Wrapper>(bvm::g_stylegan3Class, thisVal,
                                                bvm::kHostStyleGAN3Tag);
    if (!w) return ev::throwTypeError("StyleGAN3.prototype.synthesize: not a generator");
    if (!(w->loaded && w->generator)) return ev::throwError(kSg3NotLoaded);
    if (args.empty() || !ev::isObject(args[0])) {
        return ev::throwTypeError("synthesize(w, opts?): w Float32Array required");
    }

    auto job = std::make_shared<Sg3Job>();
    job->w = w;
    {
        const float* data = nullptr;
        std::size_t count = 0;
        if (!bvm::readFloat32Array(args[0], data, count) || !data) {
            return ev::throwTypeError("synthesize: w must be a Float32Array");
        }
        job->win.assign(data, data + count);
    }
    const int full = w->numWs * w->wDim;
    if (static_cast<int>(job->win.size()) != full &&
        static_cast<int>(job->win.size()) != w->wDim) {
        return ev::throwTypeError(
            "synthesize: w must have length numWs*wDim (W+) or wDim (single w)");
    }
    job->seed = -1;

    BRO_VISION_BEGIN(w, "synthesize")
    ev::Persistent optsRoot(args.size() > 1 ? args[1] : ev::undefined());
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        auto* g = job->w;
        brotensor::DeviceScope scope(g->device);
        brotensor::Tensor ws = brotensor::Tensor::mat(g->numWs, g->wDim);
        const bool single = static_cast<int>(job->win.size()) == g->wDim;
        for (int r = 0; r < g->numWs; ++r) {
            for (int c = 0; c < g->wDim; ++c) {
                ws[r * g->wDim + c] =
                    job->win[static_cast<std::size_t>(single ? c : r * g->wDim + c)];
            }
        }
        if (g->device != brotensor::Device::CPU) ws = ws.to(g->device);
        job->out.img = g->generator->render(ws);
        job->out.seed = -1;
    };
    auto build = [job]() -> Value { return sg3Result(*job->w, job->out); };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

Value sgInvert(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::StyleGAN3Wrapper>(bvm::g_stylegan3Class, thisVal,
                                                bvm::kHostStyleGAN3Tag);
    if (!w) return ev::throwTypeError("StyleGAN3.prototype.invert: not a generator");
    if (args.empty()) return ev::throwTypeError("invert(image, opts?): image required");

    auto job = std::make_shared<Sg3Job>();
    job->w = w;
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err)) {
        return ev::throwTypeError(std::string("invert: ") + err);
    }
    if (job->inW != w->imgResolution || job->inH != w->imgResolution) {
        return ev::throwTypeError(
            "invert: image must be square at the model resolution; "
            "resize the source first");
    }

    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    visionIntOpt(optsRoot.get(), "steps", job->steps);
    visionFloatOpt(optsRoot.get(), "lr", job->lr);
    visionFloatOpt(optsRoot.get(), "regW", job->regW);
    visionFloatOpt(optsRoot.get(), "initNoise", job->initNoise);
    visionInt64Opt(optsRoot.get(), "seed", job->seed);
    if (job->steps < 1) job->steps = 1;
    if (visionFloatsOpt(optsRoot.get(), "initW", job->initW) && !job->initW.empty() &&
        static_cast<int>(job->initW.size()) != w->numWs * w->wDim) {
        return ev::throwTypeError("invert: opts.initW must have length numWs*wDim");
    }
    if (!(w->loaded && w->generator)) return ev::throwError("invert: no weights loaded");

    BRO_VISION_BEGIN(w, "invert")
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        auto* g = job->w;
        brotensor::DeviceScope scope(g->device);
        sg3::Image target;
        target.width = job->inW;
        target.height = job->inH;
        target.channels = 3;
        target.rgb.resize(static_cast<std::size_t>(job->inW) * job->inH * 3);
        for (std::size_t i = 0, n = static_cast<std::size_t>(job->inW) * job->inH; i < n; ++i) {
            target.rgb[3 * i + 0] = job->rgba[4 * i + 0];
            target.rgb[3 * i + 1] = job->rgba[4 * i + 1];
            target.rgb[3 * i + 2] = job->rgba[4 * i + 2];
        }

        sg3::Generator::InvertOptions io;
        io.num_steps = job->steps;
        io.lr = job->lr;
        io.reg_w = job->regW;
        io.init_noise = job->initNoise;
        io.seed = static_cast<uint64_t>(job->seed);
        if (!job->initW.empty()) {
            io.init_w = brotensor::Tensor::from_host_on(g->device, job->initW.data(),
                                                        g->numWs, g->wDim);
        }
        job->out.lossCurve.reserve(static_cast<std::size_t>(job->steps));
        io.on_step = [job](int, float l) { job->out.lossCurve.push_back(l); };

        sg3::Generator::InvertResult r = g->generator->invert(target, io);
        brotensor::Tensor wc = r.w.to(brotensor::Device::CPU);
        const float* p = wc.host_f32();
        job->out.ws.assign(p, p + static_cast<std::size_t>(g->numWs) * g->wDim);
        job->out.img = g->generator->render(r.w);
        job->out.seed = -1;
        job->out.hasLoss = true;
        job->out.loss = r.loss;
    };
    auto build = [job]() -> Value { return sg3Result(*job->w, job->out); };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

}  // namespace

void installVisionGenerativeOps() {
    {
        ObjectBuilder proto(bvm::g_birefnetClass.prototype());
        proto.def("removeBackground", 2, birefnetRemoveBackground);
        // `estimate` is the bronze port's name for the same body.
        proto.def("estimate", 2, birefnetRemoveBackground);
    }
    {
        ObjectBuilder proto(bvm::g_stylegan3Class.prototype());
        proto.def("generate", 1, sgGenerate);
        proto.def("synthesize", 2, sgSynthesize);
        proto.def("invert", 2, sgInvert);
    }
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
