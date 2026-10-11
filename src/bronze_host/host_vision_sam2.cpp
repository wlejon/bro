// Sam2 / Sam2Tracker on bro's job machine.
//
// The image encode (setImage, Sam2Tracker.addFrame) and the memory-
// conditioned decode (prompt / track / propagate) all run on a worker when
// `onDone` is given; segment() and result() too, so the whole surface takes
// `onDone` uniformly. The prompt reader, the model calls and the result
// shapes are brovisionml's (src/api/native_vision_sam2.cpp); what bro adds
// is the thread and a drawable `image` beside every mask's bytes.
//
// One op at a time per model: a tracker shares its model's GPU workspaces,
// so the busy guard of both classes is keyed on the model.

#if BRO_WITH_VISION

#include "bronze_host/host_vision_ops_internal.h"

#include <functional>
#include <memory>
#include <utility>

namespace bro::bronze_host {

namespace {

constexpr uint8_t kMaskR = 30;
constexpr uint8_t kMaskG = 144;
constexpr uint8_t kMaskB = 255;

Value maskBitmap(const float* logits, int w, int h) {
    std::vector<uint8_t> bin(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < bin.size(); ++i) bin[i] = logits[i] > 0.0f ? 1 : 0;
    return visionBitmapMask(bin.data(), w, h, kMaskR, kMaskG, kMaskB);
}

Value frameWithImage(const brovisionml::Sam2FrameResult& r) {
    ev::Persistent o(bvm::buildSam2Frame(r));
    if (!r.logits.empty()) {
        ev::Persistent bmp(maskBitmap(r.logits.data(), r.width, r.height));
        o.set(ev::setProperty(o.get(), "image", bmp.get()));
    } else {
        o.set(ev::setProperty(o.get(), "image", ev::null()));
    }
    return o.get();
}

bvm::Sam2Wrapper* sam2Of(Value thisVal) { return bvm::sam2Self(thisVal); }

const void* busyKey(const std::shared_ptr<brovisionml::Sam2>& sam) { return sam.get(); }

// ── Sam2.setImage(image, opts?) ─────────────────────────────────────────────

struct SetImageJob {
    std::shared_ptr<brovisionml::Sam2> sam;
    brotensor::Device device = brotensor::Device::CPU;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};

Value sam2SetImage(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* w = sam2Of(thisVal);
    if (!w) return ev::throwTypeError("Sam2.prototype.setImage: not a Sam2 instance");
    if (args.empty()) return ev::throwTypeError("setImage(image, opts?): image is required");
    auto job = std::make_shared<SetImageJob>();
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->w, job->h, err))
        return ev::throwTypeError("setImage: " + err);
    if (!(w->loaded && w->sam)) return ev::throwError("Sam2.setImage: model is not loaded");
    job->sam = w->sam;
    job->device = w->device;

    const void* key = busyKey(w->sam);
    BRO_VISION_BEGIN(key, "setImage")
    ev::Persistent optsRoot(args.size() > 1 ? args[1] : ev::undefined());
    Value onDone = visionOnDone(optsRoot.get());
    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->device);
        job->sam->set_image(job->rgba.data(), job->w, job->h, 4);
    };
    auto build = []() -> Value { return ev::undefined(); };
    auto release = [key]() { visionClearBusy(key); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build), std::move(release));
}

// ── Sam2.segment(opts) ──────────────────────────────────────────────────────

struct SegmentJob {
    std::shared_ptr<brovisionml::Sam2> sam;
    brotensor::Device device = brotensor::Device::CPU;
    brovisionml::Sam2Prompt prompt;
    bool multimask = true;
    std::vector<float> maskInput;
    brovisionml::Sam2Segmentation seg;
    std::vector<float> lowRes;
};

Value sam2Segment(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* w = sam2Of(thisVal);
    if (!w) return ev::throwTypeError("Sam2.prototype.segment: not a Sam2 instance");
    auto job = std::make_shared<SegmentJob>();
    ev::Persistent optsRoot(args.empty() ? ev::undefined() : args[0]);
    std::string err;
    if (!bvm::readSam2Prompt(optsRoot.get(), job->prompt, err))
        return ev::throwTypeError("segment: " + err);
    job->multimask = visionBoolOpt(optsRoot.get(), "multimask", true);
    visionFloatsOpt(optsRoot.get(), "maskInput", job->maskInput);
    if (!(w->loaded && w->sam)) return ev::throwError("Sam2.segment: model is not loaded");
    if (!w->sam->has_image()) return ev::throwError("segment: call setImage() before segment()");
    job->sam = w->sam;
    job->device = w->device;

    const void* key = busyKey(w->sam);
    BRO_VISION_BEGIN(key, "segment")
    Value onDone = visionOnDone(optsRoot.get());
    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->device);
        job->seg = bvm::sam2Segment(*job->sam, job->prompt, job->multimask,
                                    job->maskInput.empty() ? nullptr : &job->maskInput, job->lowRes);
    };
    auto build = [job]() -> Value {
        const auto& seg = job->seg;
        ev::Persistent res(bvm::buildSam2Segmentation(seg, job->lowRes));
        ev::Persistent masks(ev::getProperty(res.get(), "masks"));
        const std::size_t plane = static_cast<std::size_t>(seg.width) * static_cast<std::size_t>(seg.height);
        for (int m = 0; m < seg.num; ++m) {
            ev::Persistent bmp(maskBitmap(seg.logits.data() + static_cast<std::size_t>(m) * plane,
                                          seg.width, seg.height));
            ev::Persistent mo(ev::getElement(masks.get(), static_cast<uint32_t>(m)));
            ev::setProperty(mo.get(), "image", bmp.get());
        }
        return res.get();
    };
    auto release = [key]() { visionClearBusy(key); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build), std::move(release));
}

// ── Sam2Tracker ─────────────────────────────────────────────────────────────

struct TrackerJob {
    std::shared_ptr<brovisionml::Sam2> sam;   // keeps the weights alive
    brovisionml::Sam2Tracker* tracker = nullptr;
    brotensor::Device device = brotensor::Device::CPU;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    int frame = -1;
    bool reverse = false;
    brovisionml::Sam2Prompt prompt;
    int index = -1;
    std::vector<brovisionml::Sam2FrameResult> results;
};

// Validates the frame index argument against the frames added so far.
bool readFrame(bvm::Sam2TrackerWrapper* t, Value v, int& frame, std::string& err) {
    if (!ev::isNumber(v)) { err = "frame index must be a number"; return false; }
    const double d = ev::toDouble(v);
    if (!(d >= 0 && d < t->tracker->num_frames()) || d != static_cast<double>(static_cast<int>(d))) {
        err = "frame index out of range";
        return false;
    }
    frame = static_cast<int>(d);
    return true;
}

std::shared_ptr<TrackerJob> trackerJob(bvm::Sam2TrackerWrapper* t) {
    auto job = std::make_shared<TrackerJob>();
    job->sam = t->sam;
    job->tracker = t->tracker.get();
    job->device = t->device;
    return job;
}

// Runs `body` (filling job->results / job->index) under the model's busy
// guard and returns what `shape` says: nothing, the new frame's index, one
// frame result, or all of them.
enum class Shape { None, Index, One, All };

Value runTrackerOp(const char* op, Value thisVal, Value opts, const std::shared_ptr<TrackerJob>& job,
                   std::function<void(TrackerJob&)> body, Shape shape) {
    const void* key = busyKey(job->sam);
    ev::Persistent optsRoot(opts);
    if (!visionMarkBusy(key)) {
        return ev::throwError(std::string(op) + ": an operation is already in flight on this model");
    }
    Value onDone = visionOnDone(optsRoot.get());
    auto compute = [job, body = std::move(body)](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->device);
        body(*job);
    };
    auto build = [job, shape]() -> Value {
        if (shape == Shape::None) return ev::undefined();
        if (shape == Shape::Index) return ev::fromDouble(job->index);
        if (shape == Shape::One)
            return job->results.empty() ? ev::null() : frameWithImage(job->results.front());
        return hostArrayOf(job->results.size(),
                           [&job](std::size_t i) { return frameWithImage(job->results[i]); });
    };
    auto release = [key]() { visionClearBusy(key); };
    return runVisionOp(onDone, thisVal, std::move(compute), std::move(build), std::move(release));
}

Value trkAddFrame(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.addFrame: not a Sam2Tracker");
    if (args.empty()) return ev::throwTypeError("addFrame(image, opts?): image is required");
    auto job = trackerJob(t);
    std::string err;
    if (!bvm::readImageInput(args[0], job->rgba, job->w, job->h, err))
        return ev::throwTypeError("addFrame: " + err);
    return runTrackerOp("addFrame", thisVal, args.size() > 1 ? args[1] : ev::undefined(), job,
                        [](TrackerJob& j) {
                            j.index = j.tracker->add_frame(j.rgba.data(), j.w, j.h, 4);
                        },
                        Shape::Index);
}

Value trkPrompt(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.prompt: not a Sam2Tracker");
    auto job = trackerJob(t);
    std::string err;
    if (!readFrame(t, args.empty() ? ev::undefined() : args[0], job->frame, err))
        return ev::throwRangeError("prompt: " + err);
    if (!bvm::readSam2Prompt(args.size() > 1 ? args[1] : ev::undefined(), job->prompt, err))
        return ev::throwTypeError("prompt: " + err);
    return runTrackerOp("prompt", thisVal, args.size() > 1 ? args[1] : ev::undefined(), job,
                        [](TrackerJob& j) {
                            j.results.push_back(j.tracker->prompt(j.frame, j.prompt));
                        },
                        Shape::One);
}

Value trkTrack(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.track: not a Sam2Tracker");
    auto job = trackerJob(t);
    std::string err;
    if (!readFrame(t, args.empty() ? ev::undefined() : args[0], job->frame, err))
        return ev::throwRangeError("track: " + err);
    job->reverse = args.size() > 1 && visionBoolOpt(args[1], "reverse", false);
    return runTrackerOp("track", thisVal, args.size() > 1 ? args[1] : ev::undefined(), job,
                        [](TrackerJob& j) {
                            j.results.push_back(j.tracker->track(j.frame, j.reverse));
                        },
                        Shape::One);
}

Value trkPropagate(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.propagate: not a Sam2Tracker");
    auto job = trackerJob(t);
    return runTrackerOp("propagate", thisVal, args.empty() ? ev::undefined() : args[0], job,
                        [](TrackerJob& j) { j.results = j.tracker->propagate(); }, Shape::All);
}

Value trkResult(Value thisIn, std::span<const Value> args) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.result: not a Sam2Tracker");
    auto job = trackerJob(t);
    std::string err;
    if (!readFrame(t, args.empty() ? ev::undefined() : args[0], job->frame, err))
        return ev::throwRangeError("result: " + err);
    return runTrackerOp("result", thisVal, args.size() > 1 ? args[1] : ev::undefined(), job,
                        [](TrackerJob& j) { j.results.push_back(j.tracker->result(j.frame)); },
                        Shape::One);
}

Value trkReset(Value thisIn, std::span<const Value>) {
    const Rooted thisVal(thisIn);
    auto* t = bvm::sam2TrackerSelf(thisVal);
    if (!t) return ev::throwTypeError("Sam2Tracker.prototype.reset: not a Sam2Tracker");
    auto job = trackerJob(t);
    return runTrackerOp("reset", thisVal, ev::undefined(), job,
                        [](TrackerJob& j) { j.tracker->reset(); }, Shape::None);
}

}  // namespace

void installVisionSam2Ops() {
    {
        ObjectBuilder proto(bvm::g_sam2Class.prototype());
        proto.def("setImage", 2, sam2SetImage);
        proto.def("segment", 1, sam2Segment);
    }
    ObjectBuilder proto(bvm::g_sam2TrackerClass.prototype());
    proto.def("addFrame", 2, trkAddFrame);
    proto.def("prompt", 3, trkPrompt);
    proto.def("track", 2, trkTrack);
    proto.def("propagate", 1, trkPropagate);
    proto.def("result", 2, trkResult);
    proto.def("reset", 0, trkReset);
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
