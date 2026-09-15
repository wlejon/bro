#include "natives/motion/native_motion_decl.h"
#include <vector>
#include <string>
#include <cstdint>

namespace {

struct BroArdyMotionPipelineImpl {
    std::string device = "CPU";
};

struct MotionGenerateSlot {
    int32_t frames = 0;
    int32_t joints = 22;
    double fps = 30.0;
    std::vector<float> positions;
    std::vector<int32_t> parents;
    std::vector<float> footContacts;
};

static thread_local MotionGenerateSlot tl_motionSlot;

} // namespace

extern "C" {

void bro_motion_ArdyMotionPipeline_dtor(void* self) {
    delete static_cast<BroArdyMotionPipelineImpl*>(self);
}

void* bro_motion_ArdyMotionPipeline_ctor(void) {
    return new BroArdyMotionPipelineImpl();
}

const char* bro_motion_ArdyMotionPipeline_device_get(void* self) {
    auto* p = static_cast<BroArdyMotionPipelineImpl*>(self);
    return p ? p->device.c_str() : "CPU";
}

void bro_motion_ArdyMotionPipeline_generate(void* /*self*/, const char* /*text*/,
                                           int32_t opts_frames, int32_t /*opts_steps*/,
                                           double /*opts_cfg*/, int32_t /*opts_seed*/,
                                           double /*opts_heading*/) {
    tl_motionSlot.frames = opts_frames > 0 ? opts_frames : 0;
    tl_motionSlot.joints = 22;
    tl_motionSlot.fps = 30.0;
    tl_motionSlot.positions.clear();
    tl_motionSlot.parents.clear();
    tl_motionSlot.footContacts.clear();
}

int32_t bro_motion_ArdyMotionPipeline_generate_frames(void) {
    return tl_motionSlot.frames;
}

int32_t bro_motion_ArdyMotionPipeline_generate_joints(void) {
    return tl_motionSlot.joints;
}

double bro_motion_ArdyMotionPipeline_generate_fps(void) {
    return tl_motionSlot.fps;
}

void bro_motion_ArdyMotionPipeline_generate_positions(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_motionSlot.positions.data();
    out->length = static_cast<uint32_t>(tl_motionSlot.positions.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

void bro_motion_ArdyMotionPipeline_generate_parents(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_motionSlot.parents.data();
    out->length = static_cast<uint32_t>(tl_motionSlot.parents.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

void bro_motion_ArdyMotionPipeline_generate_footContacts(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_motionSlot.footContacts.data();
    out->length = static_cast<uint32_t>(tl_motionSlot.footContacts.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

#if BRO_WITH_DIFFUSION && BRO_WITH_LM
void bro_motion_init(void) {}

void* bro_motion_load(bool /*opts_checkpoint_given*/, const char* /*opts_checkpoint*/,
                      bool /*opts_textEncoder_given*/, const char* /*opts_textEncoder*/,
                      bool opts_device_given, const char* opts_device) {
    auto* p = new BroArdyMotionPipelineImpl();
    if (opts_device_given && opts_device) {
        p->device = opts_device;
    }
    return p;
}
#endif

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_motion(std::string* error);
bool registerMotionNatives(std::string* error) {
    return registerNatives_motion(error);
}
} // namespace bro::bronze_host
