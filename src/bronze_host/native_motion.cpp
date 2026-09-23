// native_motion.cpp — bro.motion: ARDY-G1 text-to-motion over brodiffusion's
// ardy pipeline and brolm's LLM2Vec text encoder. The C entry points are the
// ones natives/motion/native_motion_decl.h declares; the dictionary result of
// generate() sits in a per-thread slot that js/motion.js reads member by
// member right after the call.
//
// The pipeline is a port of the QuickJS-era motion_bindings.cpp: load() builds
// the denoiser + FSQ decoder + generator from an ARDY checkpoint directory and
// the tokenizer + encoder from a merged LLM2Vec directory; generate() embeds the
// prompt, samples the hybrid token sequence window by window, detokenizes it to
// explicit ARDY features and inverts those to posed joints + foot contacts.
// Everything is synchronous (docs/motion-api.js: run it in a Worker).

#include "natives/motion/native_motion_decl.h"

#include "bronze_host/host_natives.h"
#include "embed/embed.h"

#include <cstdint>
#include <string>
#include <vector>

#if BRO_WITH_DIFFUSION && BRO_WITH_LM
#include "util/interrupt.h"

#include <brodiffusion/ardy/denoiser.h>
#include <brodiffusion/ardy/fsq_decoder.h>
#include <brodiffusion/ardy/motion_rep.h>
#include <brodiffusion/ardy/sampler.h>
#include <brodiffusion/ardy/text_conditioner.h>
#include <brolm/llama3_tokenizer.h>
#include <brolm/llm2vec.h>
#include <brotensor/runtime.h>
#include <brotensor/safetensors.h>
#include <brotensor/tensor.h>

#include <exception>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#endif

namespace ev = bronze::embed;

namespace {

// The MotionClip the wrapper assembles after generate(): filled by the call,
// read by the ArdyMotionPipeline_generate_<member> natives until the next one.
struct MotionGenerateSlot {
    int32_t              frames = 0;
    int32_t              joints = 0;
    double               fps    = 25.0;
    std::vector<float>   positions;
    std::vector<int32_t> parents;
    std::vector<float>   footContacts;

    void clear() {
        frames = 0;
        joints = 0;
        positions.clear();
        parents.clear();
        footContacts.clear();
    }
};

thread_local MotionGenerateSlot tl_motionSlot;

void fillBuffer(bronze_native_buffer* out, const void* data, std::size_t n) {
    if (!out) return;
    out->data    = const_cast<void*>(data);
    out->length  = static_cast<uint32_t>(n);
    out->release = nullptr;   // the runtime copies during the call
    out->ctx     = nullptr;
}

#if BRO_WITH_DIFFUSION && BRO_WITH_LM

namespace bt   = ::brotensor;
namespace st   = ::brotensor::safetensors;
namespace ardy = ::brodiffusion::ardy;

// Minimal .npy reader for the ARDY stats files: skip the header (magic + version
// + little-endian header length) and read `n` contiguous scalars.
std::size_t npyDataOffset(std::ifstream& in, const std::string& path) {
    char magic[8];
    in.read(magic, 8);
    unsigned char hl[2];
    in.read(reinterpret_cast<char*>(hl), 2);
    if (!in) throw std::runtime_error("short header in " + path);
    return 10 + (static_cast<std::size_t>(hl[0]) | (static_cast<std::size_t>(hl[1]) << 8));
}

std::vector<float> loadNpyF32(const std::string& path, int n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    in.seekg(static_cast<std::streamoff>(npyDataOffset(in, path)), std::ios::beg);
    std::vector<float> v(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    if (!in) throw std::runtime_error("short read from " + path);
    return v;
}

std::vector<float> loadNpyF64AsF32(const std::string& path, int n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    in.seekg(static_cast<std::streamoff>(npyDataOffset(in, path)), std::ios::beg);
    std::vector<double> d(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size() * sizeof(double)));
    if (!in) throw std::runtime_error("short read from " + path);
    std::vector<float> v(d.size());
    for (std::size_t i = 0; i < d.size(); ++i) v[i] = static_cast<float>(d[i]);
    return v;
}

bt::Device autoDevice() {
    if (bt::is_available(bt::Device::CUDA))  return bt::Device::CUDA;
    if (bt::is_available(bt::Device::Metal)) return bt::Device::Metal;
    return bt::Device::CPU;
}

const char* deviceName(bt::Device d) {
    switch (d.type) {
        case bt::DeviceType::CUDA:  return "CUDA";
        case bt::DeviceType::Metal: return "Metal";
        default:                    return "CPU";
    }
}

struct BroArdyMotionPipelineImpl {
    std::unique_ptr<brolm::llama3::Tokenizer>  tok;
    std::unique_ptr<brolm::llm2vec::Encoder>   enc;
    std::unique_ptr<ardy::ArdyDenoiser>        denoiser;
    std::unique_ptr<ardy::FsqMotionDecoder>    fsq;
    std::unique_ptr<ardy::ArdyMotionGenerator> gen;
    ardy::ArdyMotionRep                        rep{25.0};
    bt::Device                                 device = bt::Device::CPU;
    std::string                                deviceName = "CPU";

    bool loaded() const { return gen && enc && tok && denoiser; }
};

// Builds the whole pipeline or throws: a failed load leaves nothing behind
// (the unique_ptr members unwind), so a second attempt starts from scratch.
std::unique_ptr<BroArdyMotionPipelineImpl> loadPipeline(const std::string& ckpt,
                                                        const std::string& tenc,
                                                        const std::string& dev) {
    bt::init();
    bt::Device device = autoDevice();
    if (dev == "cpu")        device = bt::Device::CPU;
    else if (dev == "cuda")  device = bt::Device::CUDA;
    else if (dev == "metal") device = bt::Device::Metal;
    bt::set_default_device(device);

    auto w = std::make_unique<BroArdyMotionPipelineImpl>();
    w->device     = device;
    w->deviceName = deviceName(device);

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
    return w;
}

// The generation proper; throws std::runtime_error("interrupted") when the
// process is shutting down mid-run so a long clip does not hold teardown.
void runGenerate(BroArdyMotionPipelineImpl& w, const std::string& text,
                 int frames, int steps, float cfg, int seed, float heading,
                 MotionGenerateSlot& slot) {
    bt::DeviceScope scope(w.device);

    std::vector<float> feat;
    ardy::ardy_text_feat(*w.tok, *w.enc, text, feat);
    if (bro::util::interrupted()) throw std::runtime_error("interrupted");

    const int hyb = w.denoiser->hybrid_dim();
    const int fpt = w.denoiser->config().num_frames_per_token;
    const int G   = 52 / fpt;
    const int W   = w.gen->num_windows(frames);
    std::mt19937_64 rng(static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)));
    std::normal_distribution<float> norm(0.0f, 1.0f);
    std::vector<float> noise(static_cast<std::size_t>(W) * G * hyb);
    for (float& v : noise) v = norm(rng);

    std::vector<float> hybrid;
    int T_tok = 0;
    w.gen->generate_hybrid(feat.data(), frames, heading, steps, cfg,
                           noise.data(), hybrid, T_tok);
    if (bro::util::interrupted()) throw std::runtime_error("interrupted");

    std::vector<float> motion;
    w.gen->detokenize_to_motion(hybrid.data(), T_tok, motion);
    const int F = T_tok * fpt;

    std::vector<double> mdbl(motion.begin(), motion.end());
    ardy::ArdyMotionRep::Decoded dec = w.rep.inverse(mdbl.data(), F, /*is_normalized=*/true);
    const int J = ardy::ArdyMotionRep::kNumJoints;

    slot.frames = F;
    slot.joints = J;
    slot.fps    = w.rep.fps();

    slot.positions.assign(static_cast<std::size_t>(F) * J * 3, 0.0f);
    for (std::size_t i = 0; i < slot.positions.size() && i < dec.posed_joints.size(); ++i)
        slot.positions[i] = static_cast<float>(dec.posed_joints[i]);

    slot.footContacts.assign(static_cast<std::size_t>(F) * 4, 0.0f);
    for (std::size_t i = 0; i < slot.footContacts.size() && i < dec.foot_contacts.size(); ++i)
        slot.footContacts[i] = static_cast<float>(dec.foot_contacts[i]);

    slot.parents.resize(static_cast<std::size_t>(J));
    const int* jp = ardy::G1Skeleton::joint_parents();
    for (int j = 0; j < J; ++j) slot.parents[static_cast<std::size_t>(j)] = jp[j];
}

#else  // !(BRO_WITH_DIFFUSION && BRO_WITH_LM)

// The class still registers on a build without the tower (the wrapper chains
// its prototype unconditionally); no native ever hands out an instance.
struct BroArdyMotionPipelineImpl {
    std::string deviceName = "CPU";
    bool loaded() const { return false; }
};

#endif

}  // namespace

extern "C" {

void bro_motion_ArdyMotionPipeline_dtor(void* self) {
    delete static_cast<BroArdyMotionPipelineImpl*>(self);
}

void* bro_motion_ArdyMotionPipeline_ctor(void) {
    // Never reached from the wrapper (it refuses `new`); instances come from load().
    return nullptr;
}

const char* bro_motion_ArdyMotionPipeline_device_get(void* self) {
    auto* p = static_cast<BroArdyMotionPipelineImpl*>(self);
    return p ? p->deviceName.c_str() : "CPU";
}

void bro_motion_ArdyMotionPipeline_generate(void* self, const char* text,
                                           int32_t opts_frames, int32_t opts_steps,
                                           double opts_cfg, int32_t opts_seed,
                                           double opts_heading) {
    tl_motionSlot.clear();
    auto* p = static_cast<BroArdyMotionPipelineImpl*>(self);
    if (!p || !p->loaded()) {
        ev::throwTypeError("motion: pipeline not loaded");
        return;
    }
#if BRO_WITH_DIFFUSION && BRO_WITH_LM
    const int frames = opts_frames < 1 ? 1 : opts_frames;
    const int steps  = opts_steps  < 1 ? 1 : opts_steps;
    try {
        runGenerate(*p, text ? text : "", frames, steps,
                    static_cast<float>(opts_cfg), opts_seed,
                    static_cast<float>(opts_heading), tl_motionSlot);
    } catch (const std::exception& e) {
        tl_motionSlot.clear();
        ev::throwError(std::string("motion.generate failed: ") + e.what());
        return;
    }
#else
    (void)text; (void)opts_frames; (void)opts_steps; (void)opts_cfg; (void)opts_seed; (void)opts_heading;
    ev::throwError("bro.motion: compiled without BRO_WITH_DIFFUSION+BRO_WITH_LM");
#endif
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
    fillBuffer(out, tl_motionSlot.positions.data(), tl_motionSlot.positions.size());
}

void bro_motion_ArdyMotionPipeline_generate_parents(bronze_native_buffer* out) {
    fillBuffer(out, tl_motionSlot.parents.data(), tl_motionSlot.parents.size());
}

void bro_motion_ArdyMotionPipeline_generate_footContacts(bronze_native_buffer* out) {
    fillBuffer(out, tl_motionSlot.footContacts.data(), tl_motionSlot.footContacts.size());
}

#if BRO_WITH_DIFFUSION && BRO_WITH_LM

void bro_motion_init(void) {
    try {
        bt::init();
    } catch (const std::exception& e) {
        ev::throwError(std::string("motion.init failed: ") + e.what());
    }
}

void* bro_motion_load(bool opts_checkpoint_given, const char* opts_checkpoint,
                      bool opts_textEncoder_given, const char* opts_textEncoder,
                      bool opts_device_given, const char* opts_device) {
    // The wrapper only refuses `undefined`; a non-object (load(42)) reaches
    // here with nothing given, and an empty path is as absent as a missing one.
    const bool haveCkpt = opts_checkpoint_given && opts_checkpoint && *opts_checkpoint;
    const bool haveTenc = opts_textEncoder_given && opts_textEncoder && *opts_textEncoder;
    if (!haveCkpt || !haveTenc) {
        ev::throwTypeError("load({ checkpoint, textEncoder }): checkpoint and textEncoder "
                           "paths (strings) are required");
        return nullptr;
    }
    const std::string ckpt = opts_checkpoint;
    const std::string tenc = opts_textEncoder;
    const std::string dev  = (opts_device_given && opts_device) ? opts_device : "";
    try {
        return loadPipeline(ckpt, tenc, dev).release();
    } catch (const std::exception& e) {
        ev::throwError(std::string("motion.load failed: ") + e.what());
        return nullptr;
    }
}

#endif  // BRO_WITH_DIFFUSION && BRO_WITH_LM

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_motion(std::string* error);
bool registerMotionNatives(std::string* error) {
    return registerNatives_motion(error);
}
}  // namespace bro::bronze_host
