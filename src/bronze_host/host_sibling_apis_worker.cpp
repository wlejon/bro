// Worker-realm sibling JavaScript API installers and lifecycle hooks.
// See installSiblingApis (host_sibling_apis.cpp) for main-realm installers.

#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_natives.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#if BRO_WITH_GAMEAI
#include <brogameagent/api.h>
#endif
#if BRO_WITH_3D
#include <bromesh/api.h>
#endif
#if BRO_WITH_TENSOR
#include <brotensor/api.h>
#endif
#if BRO_WITH_LM
#include <brolm/api.h>
#endif
#include "api/api.h"  // brokit::api::resolveAssetPath

#if BRO_WITH_SOUNDML
#include <brosoundml/api.h>
#endif
#if BRO_WITH_DIFFUSION
#include <brodiffusion/api.h>
#endif
#if BRO_WITH_VISION
#include <brovisionml/api.h>
#include "bronze_host/host_vision.h"
#endif
#if BRO_WITH_FLORA
#include <broflora/api/api.h>
#endif
#if BRO_WITH_KERNEL
#include <brokernel/api.h>
#if BRO_WITH_TENSOR
#include "bronze_host/host_kernel_tensor.h"
#endif
#endif
#if BRO_WITH_THEMES
#include <brothemes/api.h>
#endif
#if BRO_WITH_SEARCH
#include <brosearch/api.h>
#endif
#include <broimage/api.h>
#include "render/image_codecs.h"

namespace bro::bronze_host {

// The Worker realm's share of the same list (host_natives.h). Every
// installer below is one whose state is per thread: a sibling's HostClass
// keeps its constructor and prototype per thread (each sibling's
// host_class.h), its install guards are thread_local, and bronze's native
// registry is per thread, so each call here builds the calling worker's own
// classes over the calling worker's own `bro` root and hands out nothing
// the main realm made. What is NOT here is everything that reaches the
// engine: broaudio's AudioContext and bro.mic (bro.ear and SynthGraph, which
// are pure computation, are here; a worker renders a graph offline but cannot
// play one), the listen tenants of
// brosoundml (wake / kws / sense / gesture / listen, which tap the engine's
// audio and inference scheduler) and the nav-mesh hooks (set once by the
// main realm; the process-global hook slots are already filled when a
// worker's bakeNavMesh reads them).
void installWorkerSiblingApis() {
#if BRO_WITH_AUDIO
    broaudio::api::installEar();
    broaudio::api::installSynthGraph();
#endif
#if BRO_WITH_GAMEAI
    brogameagent::api::installGameAi();
#endif
#if BRO_WITH_3D
    bromesh::api::setPathResolver(&brokit::api::resolveAssetPath);
    bromesh::api::installMesh();
    bromesh::api::installRigging();
#endif
#if BRO_WITH_TENSOR
    brotensor::api::setPathResolver(&brokit::api::resolveAssetPath);
    brotensor::api::installTensor();
    adoptGlobalProperty("GpuTensor");
#endif
#if BRO_WITH_LM
    brolm::api::setPathResolver(&brokit::api::resolveAssetPath);
    brolm::api::installLM();
    for (const char* name : {"AsyncHandle", "QwenTokenizer", "MistralTokenizer", "GemmaTokenizer",
                             "Llama3Tokenizer", "LMModel", "Qwen35Model", "Qwen3VLModel", "NllbModel",
                             "ClipModel", "T5Model", "LayaModel", "ModernBertModel", "Grammar"}) {
        adoptGlobalProperty(name);
    }
#endif
#if BRO_WITH_SOUNDML
    // The path resolver and log hook are process-global and already set by
    // the main realm's install; the audio engine and scheduler are not
    // consulted by the compute classes.
    brosoundml::api::installSoundMLCompute();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::setPathResolver(&brokit::api::resolveAssetPath);
    brodiffusion::api::installDiffusion();
#endif
#if BRO_WITH_VISION
    brovisionml::api::setPathResolver(&brokit::api::resolveAssetPath);
    brovisionml::api::installVision();
    // No engine in a worker realm: the jobs this realm launches are drained
    // by tickWorkerSiblingApis below, not by the frame pump.
    installVisionHostOps(nullptr);
#endif
#if BRO_WITH_FLORA
    broflora::api::installFlora();
    adoptGlobalProperty("FloraWorld");
#endif
#if BRO_WITH_KERNEL
    brokernel::api::installKernel();
#if BRO_WITH_TENSOR
    installKernelTensorInterop();
#endif
#endif
    broimage::api::setPathResolver(&brokit::api::resolveAssetPath);
    render::registerImageCodecs();
    broimage::api::installImage();
#if BRO_WITH_THEMES
    bro::themes::api::setPathResolver(&brokit::api::resolveAssetPath);
    bro::themes::api::installThemes();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::installSearch();
#endif
}

void tickWorkerSiblingApis() {
#if BRO_WITH_VISION
    // This thread's vision jobs only — the registry is per thread, because a
    // job's callbacks belong to the realm that launched it.
    tickVisionJobs();
#endif
#if BRO_WITH_LM
    brolm::api::tickLMAsync();
#endif
#if BRO_WITH_SOUNDML
    // This thread's async jobs only (a job's callbacks belong to the realm
    // that launched it).
    brosoundml::api::tickSoundMLAsync();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::tickDiffusionAsync();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::tickSearchAsync();
#endif
}

void shutdownWorkerSiblingApis() {
#if BRO_WITH_DIFFUSION
    // This thread's diffusion jobs: cancelled and joined while the worker's
    // realm (their callbacks' home) still exists.
    brodiffusion::api::shutdownDiffusionAsync();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::shutdownSearchAsync();
#endif
}

}  // namespace bro::bronze_host
