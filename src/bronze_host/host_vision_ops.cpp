// DepthEstimator and NormalEstimator on bro's job machine.
//
// Both were `estimate(image, opts?)` returning typed-array planes; both get
// their `image` ImageBitmap back and an `opts.onDone` that moves the model
// pass off the JS thread. Everything else — the option names, the plane
// names, the unloaded-model placeholder, the error text — is the sibling's,
// read from brovisionml/src/api/native_vision_models.cpp so the two agree.

#if BRO_WITH_VISION

#include "bronze_host/host_vision_ops_internal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// DepthEstimator.estimate(image, opts?)
//   -> { width, height, depth, gray, min, max, image }
// ---------------------------------------------------------------------------

struct DepthJob {
    bvm::DepthEstimatorWrapper* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 512;
    int inH = 512;
    bool invert = false;
    bool live = false;  // weights loaded AND the image decoded
    brovisionml::depth::DepthMap dm;
};

Value depthEstimate(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::DepthEstimatorWrapper>(bvm::g_depthEstimatorClass, thisVal,
                                                     bvm::kHostDepthEstimatorTag);
    if (!w) {
        return ev::throwTypeError("DepthEstimator.prototype.estimate: not a DepthEstimator");
    }

    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    auto job = std::make_shared<DepthJob>();
    job->w = w;
    job->invert = visionBoolOpt(opts, "invert", false);

    std::string err;
    const bool decoded =
        !args.empty() && bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err);
    job->live = decoded && w->loaded && w->estimator;

    BRO_VISION_BEGIN(w, "estimate")
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->dm = job->w->estimator->estimate(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        // The sibling's probe answer: a flat half-depth plane at the size it
        // knows, so a surface check works with no weights on disk.
        job->dm.width = job->inW;
        job->dm.height = job->inH;
        job->dm.depth.assign(static_cast<std::size_t>(job->inW) * job->inH, 0.5f);
    };

    auto build = [job]() -> Value {
        const auto& dm = job->dm;
        float lo = 0.0f;
        float hi = 1.0f;
        ev::Persistent bmp(
            visionBitmapGrayNormalized(dm.depth, dm.width, dm.height, job->invert, lo, hi));

        // `gray` is the same normalization as the bitmap, as bytes — the
        // plane the sibling hands a caller that rasterizes for itself.
        const float span = (hi > lo) ? (hi - lo) : 1.0f;
        std::vector<uint8_t> gray(dm.depth.size());
        for (std::size_t i = 0; i < dm.depth.size(); ++i) {
            float t = (dm.depth[i] - lo) / span;
            if (job->invert) t = 1.0f - t;
            gray[i] = static_cast<uint8_t>(std::clamp(t * 255.0f, 0.0f, 255.0f));
        }

        ObjectBuilder res;
        res.set("width", ev::fromDouble(dm.width));
        res.set("height", ev::fromDouble(dm.height));
        {
            ev::Persistent d(bvm::makeFloat32Array(dm.depth.data(), dm.depth.size()));
            res.set("depth", d.get());
        }
        {
            ev::Persistent g(bvm::makeUint8Array(gray.data(), gray.size()));
            res.set("gray", g.get());
        }
        res.set("min", ev::fromDouble(lo));
        res.set("max", ev::fromDouble(hi));
        res.set("image", bmp.get());
        return res.get();
    };

    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// NormalEstimator.estimate(image, opts?)
//   -> { width, height, normals, normal, image }
// ---------------------------------------------------------------------------

struct NormalJob {
    bvm::NormalEstimatorWrapper* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 512;
    int inH = 512;
    bool live = false;
    bool hasIntrinsics = false;
    float fx = 0.0f;
    float fy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    brovisionml::dsine::NormalMap nm;
};

Value normalEstimate(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::NormalEstimatorWrapper>(bvm::g_normalEstimatorClass, thisVal,
                                                      bvm::kHostNormalEstimatorTag);
    if (!w) {
        return ev::throwTypeError("NormalEstimator.prototype.estimate: not a NormalEstimator");
    }

    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    auto job = std::make_shared<NormalJob>();
    job->w = w;

    // Passing fx is what switches DSINE off its fov-synthesized default, so
    // fx alone decides; fy/cx/cy ride along.
    if (ev::isObject(opts)) {
        Value fxv = ev::getProperty(opts, "fx");
        if (ev::isNumber(fxv)) {
            job->hasIntrinsics = true;
            job->fx = static_cast<float>(ev::toDouble(fxv));
            visionFloatOpt(opts, "fy", job->fy);
            visionFloatOpt(opts, "cx", job->cx);
            visionFloatOpt(opts, "cy", job->cy);
        }
    }

    std::string err;
    const bool decoded =
        !args.empty() && bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err);
    job->live = decoded && w->loaded && w->estimator;

    BRO_VISION_BEGIN(w, "estimate")
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->nm = job->hasIntrinsics
                          ? job->w->estimator->estimate(job->rgba.data(), job->inW, job->inH,
                                                        4, job->fx, job->fy, job->cx, job->cy)
                          : job->w->estimator->estimate(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->nm.width = job->inW;
        job->nm.height = job->inH;
        job->nm.normals.assign(static_cast<std::size_t>(job->inW) * job->inH * 3, 0.0f);
    };

    auto build = [job]() -> Value {
        const auto& nm = job->nm;
        ev::Persistent bmp(visionBitmapNormals(nm.normals, nm.width, nm.height));
        ObjectBuilder res;
        res.set("width", ev::fromDouble(nm.width));
        res.set("height", ev::fromDouble(nm.height));
        {
            ev::Persistent n(bvm::makeFloat32Array(nm.normals.data(), nm.normals.size()));
            res.set("normals", n.get());
            // `normal` is the bronze port's name for the same plane; both
            // spellings stay published, as they are in the sibling.
            res.set("normal", n.get());
        }
        res.set("image", bmp.get());
        return res.get();
    };

    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

}  // namespace

void installVisionDepthOps() {
    {
        ObjectBuilder proto(bvm::g_depthEstimatorClass.prototype());
        proto.def("estimate", 2, depthEstimate);
    }
    {
        ObjectBuilder proto(bvm::g_normalEstimatorClass.prototype());
        proto.def("estimate", 2, normalEstimate);
    }
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
