// Sam — setImage / segment / segmentEverything on bro's job machine.
//
// setImage (the ViT encode) and segmentEverything (the grid sweep) are the
// two expensive halves; segment is the cheap per-click decode and is here
// only so the whole class takes `onDone` uniformly. Every mask carries its
// `image` ImageBitmap again — the dodger-blue overlay the old binding drew
// (30, 144, 255) — beside the `data` bytes the sibling returns.
//
// The prompt readers below mirror brovisionml/src/api/native_vision_sam.cpp:
// the array form ([[x,y], ...]) and the object form ([{x, y, label?}, ...]),
// an explicit `labels` array winning over the per-point fields.

#if BRO_WITH_VISION

#include "bronze_host/host_vision_ops_internal.h"

#include <array>
#include <memory>
#include <utility>

namespace bro::bronze_host {

namespace {

constexpr uint8_t kMaskR = 30;
constexpr uint8_t kMaskG = 144;
constexpr uint8_t kMaskB = 255;

void readPointsAndLabels(Value v, std::vector<std::array<float, 2>>& points,
                         std::vector<int>& inlineLabels) {
    if (!bvm::isJsArray(v)) return;
    ev::Persistent arr(v);
    const uint32_t n = bvm::getJsArrayLength(arr.get());
    for (uint32_t i = 0; i < n; ++i) {
        ev::Persistent e(ev::getElement(arr.get(), i));
        if (!ev::isObject(e.get())) continue;
        if (bvm::isJsArray(e.get())) {
            const float x = static_cast<float>(ev::toDouble(ev::getElement(e.get(), 0)));
            const float y = static_cast<float>(ev::toDouble(ev::getElement(e.get(), 1)));
            points.push_back({x, y});
            inlineLabels.push_back(1);
        } else {
            const float x = static_cast<float>(ev::toDouble(ev::getProperty(e.get(), "x")));
            const float y = static_cast<float>(ev::toDouble(ev::getProperty(e.get(), "y")));
            Value lv = ev::getProperty(e.get(), "label");
            points.push_back({x, y});
            inlineLabels.push_back(ev::isNumber(lv) ? satCast<int>(ev::toDouble(lv)) : 1);
        }
    }
}

std::vector<int> readInts(Value v) {
    std::vector<int> out;
    if (!bvm::isJsArray(v)) return out;
    ev::Persistent arr(v);
    const uint32_t n = bvm::getJsArrayLength(arr.get());
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        out.push_back(satCast<int>(ev::toDouble(ev::getElement(arr.get(), i))));
    }
    return out;
}

std::vector<std::array<float, 4>> readBoxes(Value v) {
    std::vector<std::array<float, 4>> out;
    if (!bvm::isJsArray(v)) return out;
    ev::Persistent arr(v);
    const uint32_t n = bvm::getJsArrayLength(arr.get());
    for (uint32_t i = 0; i < n; ++i) {
        ev::Persistent e(ev::getElement(arr.get(), i));
        if (!ev::isObject(e.get())) continue;
        std::array<float, 4> b{};
        if (bvm::isJsArray(e.get())) {
            for (uint32_t k = 0; k < 4; ++k) {
                b[k] = static_cast<float>(ev::toDouble(ev::getElement(e.get(), k)));
            }
        } else {
            static const char* kKeys[4] = {"x1", "y1", "x2", "y2"};
            for (int k = 0; k < 4; ++k) {
                b[static_cast<std::size_t>(k)] =
                    static_cast<float>(ev::toDouble(ev::getProperty(e.get(), kKeys[k])));
            }
        }
        out.push_back(b);
    }
    return out;
}

// ---------------------------------------------------------------------------
// setImage(image, opts?) -> undefined
// ---------------------------------------------------------------------------

struct SamSetImageJob {
    bvm::SamWrapper* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 0;
    int inH = 0;
    bool live = false;
};

// Each op roots its receiver first: reading the inputs and the options
// allocates, and the receiver is what runVisionOp keeps as the job's selfRef.
Value samSetImage(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* w = visionSelf<bvm::SamWrapper>(bvm::g_samClass, thisVal, bvm::kHostSamTag);
    if (!w) return ev::throwTypeError("Sam.prototype.setImage: not a Sam instance");
    if (args.empty()) return ev::throwTypeError("setImage(image, opts?): image is required");

    auto job = std::make_shared<SamSetImageJob>();
    job->w = w;
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err)) {
        return ev::throwTypeError(std::string("setImage: ") + err);
    }
    // The sibling records the geometry before the encode, so a segment()
    // that arrives with no weights still knows the frame it is talking about.
    w->imageW = job->inW;
    w->imageH = job->inH;
    w->hasImage = true;
    job->live = w->loaded && w->sam;

    BRO_VISION_BEGIN(w, "setImage")
    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        if (!job->live) return;
        brotensor::DeviceScope scope(job->w->device);
        job->w->sam->set_image(job->rgba.data(), job->inW, job->inH, 4);
    };
    auto build = []() -> Value { return ev::undefined(); };
    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// segment(opts?) -> { num, width, height, best, masks: [{iou, data, logits, image}] }
// ---------------------------------------------------------------------------

struct SamSegmentJob {
    bvm::SamWrapper* w = nullptr;
    std::vector<std::array<float, 2>> points;
    std::vector<int> labels;
    std::vector<std::array<float, 4>> boxes;
    bool multimask = true;
    brovisionml::sam::Segmentation seg;
};

Value samSegment(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* w = visionSelf<bvm::SamWrapper>(bvm::g_samClass, thisVal, bvm::kHostSamTag);
    if (!w) return ev::throwTypeError("Sam.prototype.segment: not a Sam instance");

    auto job = std::make_shared<SamSegmentJob>();
    job->w = w;
    std::vector<int> inlineLabels;
    Value opts = args.empty() ? ev::undefined() : args[0];
    ev::Persistent optsRoot(opts);
    if (ev::isObject(optsRoot.get())) {
        readPointsAndLabels(ev::getProperty(optsRoot.get(), "points"), job->points,
                            inlineLabels);
        job->labels = readInts(ev::getProperty(optsRoot.get(), "labels"));
        job->boxes = readBoxes(ev::getProperty(optsRoot.get(), "boxes"));
        Value mm = ev::getProperty(optsRoot.get(), "multimask");
        if (!ev::isUndefined(mm) && !ev::isNull(mm)) job->multimask = ev::toBool(mm);
    }
    if (job->labels.empty()) job->labels = inlineLabels;
    if (job->labels.size() < job->points.size()) job->labels.resize(job->points.size(), 1);

    // Validation before the model check, so a prompt-less call is a TypeError
    // whether or not weights happen to be loaded.
    if (job->points.empty() && job->boxes.empty()) {
        return ev::throwTypeError("segment: opts.points or opts.boxes required");
    }
    // Then the sibling's two refusals, in its order: no weights, no image.
    if (!(w->loaded && w->sam)) {
        return ev::throwError("Sam.segment: model is not initialized/loaded");
    }
    if (!w->sam->has_image()) {
        return ev::throwError("segment: call setImage() before segment()");
    }

    BRO_VISION_BEGIN(w, "segment")
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->w->device);
        job->seg = job->w->sam->segment(job->points, job->labels, job->boxes, job->multimask);
    };

    auto build = [job]() -> Value {
        const auto& seg = job->seg;
        const std::size_t plane =
            static_cast<std::size_t>(seg.width) * static_cast<std::size_t>(seg.height);
        ev::Persistent masks(hostArrayOf(static_cast<std::size_t>(seg.num), [&](std::size_t m) -> Value {
            std::vector<uint8_t> bin(plane);
            const float* lg = seg.logits.data() + m * plane;
            for (std::size_t i = 0; i < plane; ++i) bin[i] = lg[i] > 0.0f ? 1 : 0;
            ev::Persistent bmp(
                visionBitmapMask(bin.data(), seg.width, seg.height, kMaskR, kMaskG, kMaskB));
            ObjectBuilder mo;
            mo.set("iou", ev::fromDouble(m < seg.iou.size() ? seg.iou[m] : 0.0f));
            {
                ev::Persistent d(bvm::makeUint8Array(bin.data(), bin.size()));
                mo.set("data", d.get());
            }
            {
                ev::Persistent lo(bvm::makeFloat32Array(lg, plane));
                mo.set("logits", lo.get());
            }
            mo.set("image", bmp.get());
            return mo.get();
        }));

        ObjectBuilder res;
        res.set("num", ev::fromDouble(seg.num));
        res.set("width", ev::fromDouble(seg.width));
        res.set("height", ev::fromDouble(seg.height));
        res.set("best", ev::fromDouble(seg.num > 0 ? seg.best() : 0));
        res.set("masks", masks.get());
        return res.get();
    };

    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

// ---------------------------------------------------------------------------
// segmentEverything(image, opts?) -> { width, height, masks: [...] }
// ---------------------------------------------------------------------------

struct SamEverythingJob {
    bvm::SamWrapper* w = nullptr;
    std::vector<uint8_t> rgba;
    int inW = 0;
    int inH = 0;
    brovisionml::sam::AmgConfig cfg;
    std::vector<brovisionml::sam::GeneratedMask> masks;
};

Value samSegmentEverything(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* w = visionSelf<bvm::SamWrapper>(bvm::g_samClass, thisVal, bvm::kHostSamTag);
    if (!w) return ev::throwTypeError("Sam.prototype.segmentEverything: not a Sam instance");
    if (args.empty()) {
        return ev::throwTypeError("segmentEverything(image, opts?): image required");
    }

    auto job = std::make_shared<SamEverythingJob>();
    job->w = w;
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->inW, job->inH, err)) {
        return ev::throwTypeError(std::string("segmentEverything: ") + err);
    }

    Value opts = args.size() > 1 ? args[1] : ev::undefined();
    ev::Persistent optsRoot(opts);
    if (ev::isObject(optsRoot.get())) {
        visionIntOpt(optsRoot.get(), "pointsPerSide", job->cfg.points_per_side);
        visionIntOpt(optsRoot.get(), "pointsPerBatch", job->cfg.points_per_batch);
        visionFloatOpt(optsRoot.get(), "predIouThresh", job->cfg.pred_iou_thresh);
        visionFloatOpt(optsRoot.get(), "stabilityThresh", job->cfg.stability_score_thresh);
        visionFloatOpt(optsRoot.get(), "boxNmsThresh", job->cfg.box_nms_thresh);
        visionIntOpt(optsRoot.get(), "cropNLayers", job->cfg.crop_n_layers);
        visionIntOpt(optsRoot.get(), "minMaskRegionArea", job->cfg.min_mask_region_area);
    }
    if (!(w->loaded && w->sam)) {
        return ev::throwError("Sam.segmentEverything: model is not initialized/loaded");
    }

    BRO_VISION_BEGIN(w, "segmentEverything")
    Value onDone = visionOnDone(optsRoot.get());

    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->w->device);
        brovisionml::sam::AutomaticMaskGenerator gen(*job->w->sam, job->cfg);
        job->masks = gen.generate(job->rgba.data(), job->inW, job->inH, 4);
    };

    auto build = [job]() -> Value {
        // generate() leaves its own image cached on the model.
        job->w->hasImage = true;
        job->w->imageW = job->inW;
        job->w->imageH = job->inH;
        ev::Persistent arr(hostArrayOf(job->masks.size(), [&job](std::size_t i) -> Value {
            const auto& gm = job->masks[i];
            ev::Persistent bmp(
                visionBitmapMask(gm.mask.data(), gm.width, gm.height, kMaskR, kMaskG, kMaskB));
            ObjectBuilder mo;
            {
                ev::Persistent d(bvm::makeUint8Array(gm.mask.data(), gm.mask.size()));
                mo.set("data", d.get());
            }
            mo.set("width", ev::fromDouble(gm.width));
            mo.set("height", ev::fromDouble(gm.height));
            {
                ev::Persistent bbox(hostArrayOf(4, [&gm](std::size_t k) {
                    return ev::fromDouble(static_cast<double>(gm.bbox[k]));
                }));
                mo.set("bbox", bbox.get());
            }
            mo.set("area", ev::fromDouble(static_cast<double>(gm.area)));
            mo.set("predictedIou", ev::fromDouble(gm.predicted_iou));
            mo.set("stabilityScore", ev::fromDouble(gm.stability_score));
            {
                ev::Persistent pt(hostArrayOf(2, [&gm](std::size_t k) {
                    return ev::fromDouble(static_cast<double>(gm.point[k]));
                }));
                mo.set("point", pt.get());
            }
            mo.set("image", bmp.get());
            return mo.get();
        }));

        ObjectBuilder res;
        res.set("width", ev::fromDouble(job->inW));
        res.set("height", ev::fromDouble(job->inH));
        res.set("masks", arr.get());
        return res.get();
    };

    auto release = [w]() { visionClearBusy(w); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build),
                       std::move(release));
}

}  // namespace

void installVisionSamOps() {
    ObjectBuilder proto(bvm::g_samClass.prototype());
    proto.def("setImage", 2, samSetImage);
    proto.def("segment", 1, samSegment);
    proto.def("segmentEverything", 2, samSegmentEverything);
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
