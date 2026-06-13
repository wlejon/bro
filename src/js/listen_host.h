#pragma once

#include <broaudio/loopback_capture.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/listen_bus.h>
#include <brotensor/tensor.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace broaudio {
class Engine;
}

namespace bro::engine {
class AudioInference;
}

namespace bro::js {

// ─── ListenHost — the engine's shared listening front-ends ──────────────────
//
// A ListenStream is one independent listening pipeline: a single audio SOURCE
// (the microphone, the system-audio render mix, or one application's audio) →
// one raw (no-AGC) 16 kHz ring → one brosoundml::ListenBus (PCEN mel front-end)
// → up to one each of {SensorHub, PhonemeSpotter, WakeWord, GestureSpotter}
// attached as members. One feature pass, one forward per attached model, N
// listeners — all hearing THAT source.
//
// Multiple streams run concurrently and independently: a mic stream driving
// wake/kws for voice commands can run alongside a system-audio stream driving
// streaming STT for the audio the machine is playing, with NO mixing — each is
// its own source, ring, bus, retention and tenant set. Streams never share
// audio; spinning up one per channel is how you listen to L/R separately.
//
// Threading / membership: each stream's bus is single-producer and is ONLY
// touched from its inference-task closure. A membership change replaces the
// whole task (removeTask + addTask): AudioInference applies both commands, in
// order, on the worker between pumps, so the old closure never overlaps the new
// one and the main thread never touches the bus. The ring has exactly one
// producer — the mic-tap callback OR the LoopbackCapture callback — so SPSC
// holds. No locks anywhere. Streams are created/destroyed on the main thread.
//
// onSpots/onWake/onGestures run on the INFERENCE thread; tenants publish into
// their own SPSC delivery from there.

using ListenSpotsFn =
    std::function<void(const std::vector<brosoundml::SpotEvent>&)>;
// Invoked on the inference thread after EVERY bus feed while a wake member is
// attached (not only on fires) so the tenant can publish per-block score
// telemetry alongside the fire flag.
using ListenWakeFn = std::function<void(bool fired)>;
// Invoked on the inference thread after a bus feed when a gesture member is
// attached and at least one gesture fired this block.
using ListenGesturesFn =
    std::function<void(const std::vector<brosoundml::GestureEvent>&)>;

// What a stream listens to.
struct ListenSource {
    enum class Kind { Mic, SystemLoopback, ProcessLoopback };
    Kind          kind    = Kind::Mic;
    std::uint32_t pid     = 0;      // ProcessLoopback target
    bool          exclude = false;  // ProcessLoopback: capture all EXCEPT the tree
    int           channel = -1;     // loopback: -1 downmix all; >=0 pick one channel
};

using StreamId = std::uint32_t;
inline constexpr StreamId kInvalidStream = 0;

struct ListenRetentionInfo {
    bool         active = false;
    int          seconds = 0;
    int          rate = 0;
    int          hop = 0;
    std::int64_t streamFrame = 0;
    std::int64_t heldFrames = 0;
};

// ─── Manager lifecycle ───────────────────────────────────────────────────────

// Wire the host to the engine's subsystems (engine init) / drop everything
// (engine teardown — closes every stream and detaches all members).
void installListenHost(broaudio::Engine* audio,
                       engine::AudioInference* inference);
void shutdownListenHost();

// ─── Streams ───────────────────────────────────────────────────────────────

// Open a stream on `src`. For loopback sources the capture starts immediately
// (audio flows / can be retained without any model attached). Returns
// kInvalidStream if the source is unavailable (e.g. loopback unsupported, or
// the target process is gone). Main thread only.
StreamId listenHostOpen(const ListenSource& src);

// Close a stream: detach its members, stop its source, free its infra. Safe on
// an unknown id. Main thread only.
void listenHostClose(StreamId id);

bool listenHostValid(StreamId id);

// Is render-side (loopback / per-process) capture available on this build/OS?
bool listenHostLoopbackSupported();
// Applications currently holding a render audio session (for an app picker).
std::vector<broaudio::AudioProcess> listenHostEnumerateApps();

// ─── Per-stream member attach / detach (pass nullptr to detach) ──────────────
// Throws std::runtime_error on a front-end framing mismatch. Main thread only.

void listenStreamSetHub(StreamId id, std::shared_ptr<brosoundml::SensorHub> hub);
void listenStreamSetSpotter(StreamId id,
                            std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                            brotensor::Device device, ListenSpotsFn onSpots);
void listenStreamSetWake(StreamId id, std::shared_ptr<brosoundml::WakeWord> wake,
                         brotensor::Device device, ListenWakeFn onWake);
void listenStreamSetGesture(StreamId id,
                            std::shared_ptr<brosoundml::GestureSpotter> gesture,
                            ListenGesturesFn onGestures);

// The stream's shared tap (kInvalidMicTapId for a loopback stream or when the
// infra is down) — for the tenants' stats() surfaces.
broaudio::MicTapId listenStreamTapId(StreamId id);

// Manual feed for tests / scripted scenarios. Threaded: write the shared ring.
// Headless: run the bus synchronously on this thread for ALL attached members
// of this stream and return the result.
void listenStreamWriteRing(StreamId id, const float* samples, int n);
brosoundml::ListenFeedResult listenStreamFeedInline(StreamId id,
                                                    const float* samples, int n);

// Per-stream raw-audio retention (see the retention notes below).
void listenStreamSetRetention(StreamId id, int seconds);
std::int64_t listenStreamFrame(StreamId id);
int listenStreamReadAudio(StreamId id, std::int64_t startFrame,
                          std::int64_t endFrame, std::vector<float>& out);
ListenRetentionInfo listenStreamRetentionInfo(StreamId id);

// ─── Default mic stream (migration scaffold) ─────────────────────────────────
//
// The existing global bro.sense / bro.kws / bro.wake / bro.gesture / bro.listen
// bindings target one shared microphone stream. These wrappers create-or-get
// that stream lazily on first attach and tear its infra down when the last
// member detaches (releasing the mic when idle). They will be retired once the
// tenants move to the per-stream stream-handle API.

void listenHostSetHub(std::shared_ptr<brosoundml::SensorHub> hub);
void listenHostSetSpotter(std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                          brotensor::Device device, ListenSpotsFn onSpots);
void listenHostSetWake(std::shared_ptr<brosoundml::WakeWord> wake,
                       brotensor::Device device, ListenWakeFn onWake);
void listenHostSetGesture(std::shared_ptr<brosoundml::GestureSpotter> gesture,
                          ListenGesturesFn onGestures);

broaudio::MicTapId listenHostTapId();
void listenHostWriteRing(const float* samples, int n);
brosoundml::ListenFeedResult listenHostFeedInline(const float* samples, int n);

// ─── Stream retention (opt-in) ──────────────────────────────────────────────
//
// A ring of the RAW samples driving a stream, so recent audio can be scrubbed /
// replayed by frame range (e.g. to hear exactly what a match fired on). Source-
// agnostic: it captures whatever feeds the stream — a mic tap, a scripted
// feed(), or a system-audio / per-process loopback source. Off by default (no
// memory cost until enabled).
//
// Single-producer writer (the feed thread, at the bus chokepoint); the reader
// (main thread) copies a behind-the-head window, race-free without a lock
// because a slot is only overwritten after a full retention period.
//
// Frame axis: samples-consumed / hop — the same axis bro.sense reports. A fresh
// stream restarts the axis at 0.

void listenHostSetRetention(int seconds);
std::int64_t listenHostStreamFrame();
int listenHostReadAudio(std::int64_t startFrame, std::int64_t endFrame,
                        std::vector<float>& out);
ListenRetentionInfo listenHostRetentionInfo();

}  // namespace bro::js
