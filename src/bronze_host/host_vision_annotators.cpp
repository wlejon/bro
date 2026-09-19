// The five ControlNet annotators — HED, Lineart, MLSD, OpenPose, SegFormer —
// on bro's job machine.
//
// Each keeps both spellings the sibling publishes (`edge`/`edges`,
// `line`/`lines`, `segments`/`lines`, `bodies`/`poses`, `classes`/`segments`)
// and both method names (`detect` is the pre-transition one, `estimate` what
// the bronze port renamed it to), and each gets its `image` ImageBitmap back:
// the edge and line probabilities as gray, MLSD's segments as white strokes
// on transparent black, OpenPose through the library's own canonical
// skeleton canvas, SegFormer through the library's ADE20K palette.

#if BRO_WITH_VISION

#include "bronze_host/host_vision_ops_internal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace bro::bronze_host {

namespace {

// A unit-scalar plane as the byte plane the port's key carries.
std::vector<uint8_t> toBytes(const std::vector<float>& unit) {
    std::vector<uint8_t> out(unit.size());
    for (std::size_t i = 0; i < unit.size(); ++i) {
        out[i] = static_cast<uint8_t>(std::clamp(unit[i] * 255.0f, 0.0f, 255.0f));
    }
    return out;
}

// { width, height, <oldKey>: Float32Array, <newKey>: Uint8Array, image }
Value scalarPlaneResult(int w, int h, const std::vector<float>& plane, const char* oldKey,
                        const char* newKey) {
    const std::vector<uint8_t> bytes = toBytes(plane);
    ev::Persistent bmp(visionBitmapGrayUnit(plane, w, h, /*invert=*/false));
    ObjectBuilder res;
    res.set("width", ev::fromDouble(w));
    res.set("height", ev::fromDouble(h));
    {
        ev::Persistent f(bvm::makeFloat32Array(plane.data(), plane.size()));
        res.set(oldKey, f.get());
    }
    {
        ev::Persistent b(bvm::makeUint8Array(bytes.data(), bytes.size()));
        res.set(newKey, b.get());
    }
    res.set("image", bmp.get());
    return res.get();
}

// The launch-side state every annotator shares: the decoded pixels and
// whether there is a model to run them through.
template <typename WrapperT>
struct AnnotatorInput {
    WrapperT* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 512;
    int inH = 512;
    bool live = false;
};

template <typename WrapperT, typename DetectorPtrT>
bool fillAnnotatorInput(AnnotatorInput<WrapperT>& in, WrapperT* w, std::span<const Value> args,
                        DetectorPtrT WrapperT::*detector) {
    in.w = w;
    std::string err;
    const bool decoded =
        !args.empty() && bvm::readImageInput(args[0], in.rgba, in.inW, in.inH, err);
    in.live = decoded && w->loaded && (w->*detector) != nullptr;
    return decoded;
}

// ---------------------------------------------------------------------------
// HED — soft edges
// ---------------------------------------------------------------------------

struct HedJob : AnnotatorInput<bvm::HedWrapper> {
    brovisionml::hed::EdgeMap em;
};

Value hedDetect(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::HedWrapper>(bvm::g_hedClass, thisVal, bvm::kHostHedTag);
    if (!w) return ev::throwTypeError("SoftEdgeDetector.prototype.detect: not a detector");

    auto job = std::make_shared<HedJob>();
    fillAnnotatorInput(*job, w, args, &bvm::HedWrapper::detector);

    BRO_VISION_BEGIN(w, "detect")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->em = job->w->detector->detect(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->em.width = job->inW;
        job->em.height = job->inH;
        job->em.edge.assign(static_cast<std::size_t>(job->inW) * job->inH, 0.0f);
    };
    auto build = [job]() -> Value {
        return scalarPlaneResult(job->em.width, job->em.height, job->em.edge, "edge", "edges");
    };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// Lineart
// ---------------------------------------------------------------------------

struct LineartJob : AnnotatorInput<bvm::LineartWrapper> {
    brovisionml::lineart::LineMap lm;
};

Value lineartDetect(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::LineartWrapper>(bvm::g_lineartClass, thisVal,
                                              bvm::kHostLineartTag);
    if (!w) return ev::throwTypeError("LineartDetector.prototype.detect: not a detector");

    auto job = std::make_shared<LineartJob>();
    fillAnnotatorInput(*job, w, args, &bvm::LineartWrapper::detector);

    BRO_VISION_BEGIN(w, "detect")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->lm = job->w->detector->detect(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->lm.width = job->inW;
        job->lm.height = job->inH;
        job->lm.line.assign(static_cast<std::size_t>(job->inW) * job->inH, 0.0f);
    };
    auto build = [job]() -> Value {
        return scalarPlaneResult(job->lm.width, job->lm.height, job->lm.line, "line", "lines");
    };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// MLSD — line segments
// ---------------------------------------------------------------------------

struct MlsdJob : AnnotatorInput<bvm::MlsdWrapper> {
    brovisionml::mlsd::LineMap lm;
};

Value mlsdDetect(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::MlsdWrapper>(bvm::g_mlsdClass, thisVal, bvm::kHostMlsdTag);
    if (!w) return ev::throwTypeError("MLSDdetector.prototype.detect: not a detector");

    auto job = std::make_shared<MlsdJob>();
    fillAnnotatorInput(*job, w, args, &bvm::MlsdWrapper::detector);

    BRO_VISION_BEGIN(w, "detect")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->lm = job->w->detector->detect(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->lm.width = job->inW;
        job->lm.height = job->inH;
    };
    auto build = [job]() -> Value {
        const auto& lm = job->lm;
        std::vector<float> x1, y1, x2, y2;
        x1.reserve(lm.segments.size());
        y1.reserve(lm.segments.size());
        x2.reserve(lm.segments.size());
        y2.reserve(lm.segments.size());
        for (const auto& s : lm.segments) {
            x1.push_back(s.x1);
            y1.push_back(s.y1);
            x2.push_back(s.x2);
            y2.push_back(s.y2);
        }
        ev::Persistent bmp(visionBitmapSegments(x1, y1, x2, y2, lm.width, lm.height));
        ev::Persistent arr(hostArrayOf(lm.segments.size(), [&lm](std::size_t i) {
            const auto& s = lm.segments[i];
            ObjectBuilder o;
            o.set("x1", ev::fromDouble(s.x1));
            o.set("y1", ev::fromDouble(s.y1));
            o.set("x2", ev::fromDouble(s.x2));
            o.set("y2", ev::fromDouble(s.y2));
            o.set("score", ev::fromDouble(s.score));
            return o.get();
        }));
        ObjectBuilder res;
        res.set("width", ev::fromDouble(lm.width));
        res.set("height", ev::fromDouble(lm.height));
        res.set("segments", arr.get());
        res.set("lines", arr.get());  // the port's name for the same array
        res.set("image", bmp.get());
        return res.get();
    };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// OpenPose
// ---------------------------------------------------------------------------

struct OpenposeJob : AnnotatorInput<bvm::OpenposeWrapper> {
    brovisionml::openpose::PoseResult pose;
};

Value openposeDetect(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::OpenposeWrapper>(bvm::g_openposeClass, thisVal,
                                               bvm::kHostOpenposeTag);
    if (!w) return ev::throwTypeError("OpenposeDetector.prototype.detect: not a detector");

    auto job = std::make_shared<OpenposeJob>();
    fillAnnotatorInput(*job, w, args, &bvm::OpenposeWrapper::detector);

    BRO_VISION_BEGIN(w, "detect")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->pose = job->w->detector->detect(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->pose.width = job->inW;
        job->pose.height = job->inH;
    };
    auto build = [job]() -> Value {
        const auto& pose = job->pose;
        ev::Persistent bmp(ev::null());
        if (pose.width > 0 && pose.height > 0) {
            const std::vector<uint8_t> canvas =
                brovisionml::openpose::OpenposeDetector::draw(pose);
            if (canvas.size() >=
                static_cast<std::size_t>(pose.width) * pose.height * 3) {
                bmp.set(visionBitmapRGB(canvas.data(), pose.width, pose.height));
            }
        }
        ev::Persistent bodies(hostArrayOf(pose.bodies.size(), [&pose](std::size_t b) {
            const auto& body = pose.bodies[b];
            ev::Persistent kps(hostArrayOf(body.keypoints.size(), [&body](std::size_t k) {
                const auto& kp = body.keypoints[k];
                ObjectBuilder ko;
                ko.set("x", ev::fromDouble(kp.x));
                ko.set("y", ev::fromDouble(kp.y));
                ko.set("score", ev::fromDouble(kp.score));
                ko.set("present", ev::fromBool(kp.present));
                return ko.get();
            }));
            ObjectBuilder bo;
            bo.set("keypoints", kps.get());
            bo.set("totalScore", ev::fromDouble(body.total_score));
            bo.set("totalParts", ev::fromDouble(body.total_parts));
            bo.set("score", ev::fromDouble(body.total_score));
            return bo.get();
        }));
        ObjectBuilder res;
        res.set("width", ev::fromDouble(pose.width));
        res.set("height", ev::fromDouble(pose.height));
        res.set("bodies", bodies.get());
        res.set("poses", bodies.get());  // the port's name for the same array
        res.set("image", bmp.get());
        return res.get();
    };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// SegFormer
// ---------------------------------------------------------------------------

struct SegformerJob : AnnotatorInput<bvm::SegformerWrapper> {
    brovisionml::segformer::SegMap sm;
};

Value segformerDetect(Value thisVal, std::span<const Value> args) {
    auto* w = visionSelf<bvm::SegformerWrapper>(bvm::g_segformerClass, thisVal,
                                                bvm::kHostSegformerTag);
    if (!w) return ev::throwTypeError("SegformerDetector.prototype.detect: not a detector");

    auto job = std::make_shared<SegformerJob>();
    fillAnnotatorInput(*job, w, args, &bvm::SegformerWrapper::detector);

    BRO_VISION_BEGIN(w, "detect")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (job->live) {
            brotensor::DeviceScope scope(job->w->device);
            job->sm = job->w->detector->detect(job->rgba.data(), job->inW, job->inH, 4);
            return;
        }
        job->sm.width = job->inW;
        job->sm.height = job->inH;
        job->sm.classes.assign(static_cast<std::size_t>(job->inW) * job->inH, 0);
    };
    auto build = [job]() -> Value {
        const auto& sm = job->sm;
        ev::Persistent bmp(ev::null());
        if (sm.width > 0 && sm.height > 0 && !sm.classes.empty()) {
            const std::vector<uint8_t> rgb =
                brovisionml::segformer::SegformerDetector::colorize(sm);
            if (rgb.size() >= static_cast<std::size_t>(sm.width) * sm.height * 3) {
                bmp.set(visionBitmapRGB(rgb.data(), sm.width, sm.height));
            }
        }
        const std::vector<int32_t> asI32(sm.classes.begin(), sm.classes.end());
        ObjectBuilder res;
        res.set("width", ev::fromDouble(sm.width));
        res.set("height", ev::fromDouble(sm.height));
        {
            // `classes` is the old key: the raw class-id bytes, ids in [0, 149].
            ev::Persistent c(bvm::makeUint8Array(sm.classes.data(), sm.classes.size()));
            res.set("classes", c.get());
        }
        {
            // `segments` is the port's name, as Int32.
            ev::Persistent s(bvm::makeInt32Array(asI32.data(), asI32.size()));
            res.set("segments", s.get());
        }
        res.set("image", bmp.get());
        return res.get();
    };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// Both names for the same body, as the sibling publishes them.
void defDetect(ObjectBuilder& proto, ev::NativeFn fn) {
    proto.def("detect", 2, fn);
    proto.def("estimate", 2, fn);
}

}  // namespace

void installVisionAnnotatorOps() {
    {
        ObjectBuilder proto(bvm::g_hedClass.prototype());
        defDetect(proto, hedDetect);
    }
    {
        ObjectBuilder proto(bvm::g_lineartClass.prototype());
        defDetect(proto, lineartDetect);
    }
    {
        ObjectBuilder proto(bvm::g_mlsdClass.prototype());
        defDetect(proto, mlsdDetect);
    }
    {
        ObjectBuilder proto(bvm::g_openposeClass.prototype());
        defDetect(proto, openposeDetect);
    }
    {
        ObjectBuilder proto(bvm::g_segformerClass.prototype());
        defDetect(proto, segformerDetect);
    }
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
