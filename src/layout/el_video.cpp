#include "layout/el_video.h"

#include "render/renderer.h"

#include <cmath>

#if BRO_WITH_VIDEO

#include "broaudio/engine.h"
#include "broaudio/dsp/resampler.h"
#include "dom/element.h"
#include "dom/event.h"
#include "js/event_dispatch.h"
#include "video/audio_decoder.h"
#include "video/media_backend.h"
#include "video/video_pipeline.h"

namespace bro::layout {

using bromath::cfromColor8;

// HTMLMediaElement spec allows 4–66 Hz. 250 ms of media time is well within
// that range and matches Chromium's low-rate path.
static constexpr double kTimeUpdateIntervalSec = 0.25;


ElVideo::ElVideo(render::Renderer* renderer) : renderer_(renderer) {}
ElVideo::~ElVideo() { delete pipeline_; }

bool ElVideo::load(const std::string& path) {
    auto* p = new bro::video::VideoPipeline();
    const std::string resolved = elem_ ? elem_->resolveUrl(path) : path;
    if (!p->open(resolved)) { delete p; return false; }
    pipeline_ = p;
    currentSrc_ = resolved;
    intrinsicWidth_ = pipeline_->frameWidth() > 0 ? pipeline_->frameWidth() : intrinsicWidth_;
    intrinsicHeight_ = pipeline_->frameHeight() > 0 ? pipeline_->frameHeight() : intrinsicHeight_;
    // Apply IDL state that was set before/after the element had a pipeline:
    // rate goes to the freshly-created clock, and the "muted" content
    // attribute (reflected by defaultMuted) initializes the live muted state.
    pipeline_->setRate(playbackRate_);
    if (elem_ && elem_->hasAttribute("muted")) muted_ = true;
    // Prime the first frame so layout has something to show before play().
    pipeline_->advanceTo(0);
    pendingLoadedMetadata_ = true;
    pendingCanPlayThrough_ = true;
    endedFired_ = false;
    waiting_ = false;
    lastTimeUpdateSec_ = -1.0;

    // Predecode the audio track in parallel through an independent demuxer.
    // VideoPipeline's main source pumps video only and drops audio packets,
    // so we open the file a second time just for audio.
    if (audioEngine_ && pipeline_->audioDecoder()) {
        openAudioTrack(resolved);
    }
    return true;
}

void ElVideo::openAudioTrack(const std::string& resolvedPath) {
    // Open the file a second time for audio alone: VideoPipeline's source
    // pumps video and drops audio packets. Go through the backend registry
    // for the same reason the pipeline does — a host application's backend
    // has to be able to deliver sound, not just picture. Hardcoding a WebM
    // demuxer and an Opus decoder here would have left `<video>` silent under
    // every other backend.
    std::unique_ptr<bro::video::MediaSource> audioSource;
    const bro::video::MediaBackend* backend = nullptr;
    for (const auto& be : bro::video::mediaBackends()) {
        if (!be.open) continue;
        audioSource = be.open(resolvedPath);
        if (audioSource) { backend = &be; break; }
    }
    if (!audioSource || !backend || !backend->makeAudioDecoder) return;

    uint32_t audioTrackId = 0;
    uint32_t sampleRate = 0, channels = 0;
    std::unique_ptr<bro::video::AudioDecoder> decoder;
    for (const auto& t : audioSource->tracks()) {
        if (t.kind != bro::video::TrackKind::Audio) continue;
        decoder = backend->makeAudioDecoder(t);
        if (!decoder) continue;          // codec this backend can't decode
        audioTrackId = t.id;
        sampleRate = t.sampleRate;
        channels = t.channels;
        break;
    }
    if (!decoder || audioTrackId == 0 || sampleRate == 0 || channels == 0) return;

    // Drain audio packets → PCM. For short clips (calling-app MVP is on the
    // order of seconds) this fits comfortably in memory. Longer clips will
    // move to a streaming source fed by the decode thread.
    std::vector<float> pcm;
    pcm.reserve(static_cast<size_t>(sampleRate) * channels * 2);
    bro::video::MediaPacket pkt;
    bro::video::AudioFrame frame;
    while (audioSource->readPacket(pkt)) {
        if (pkt.trackId != audioTrackId) continue;
        if (!decoder->decode(pkt, frame)) continue;
        pcm.insert(pcm.end(), frame.samples.begin(), frame.samples.end());
    }
    if (pcm.empty()) return;

    // broaudio clips are assumed to be at the engine's sample rate; Opus is
    // typically 48 kHz and the engine default is 44.1 kHz, so resample.
    int numFrames = static_cast<int>(pcm.size() / channels);
    const int engineRate = audioEngine_->sampleRate();
    if (static_cast<int>(sampleRate) != engineRate) {
        auto resampled = broaudio::resample(pcm.data(), numFrames,
                                            static_cast<int>(channels),
                                            static_cast<int>(sampleRate),
                                            engineRate);
        if (resampled.empty()) return;
        numFrames = static_cast<int>(resampled.size() / channels);
        audioClipId_ = audioEngine_->createClip(resampled.data(), numFrames,
                                                 static_cast<int>(channels));
    } else {
        audioClipId_ = audioEngine_->createClip(pcm.data(), numFrames,
                                                 static_cast<int>(channels));
    }
}

void ElVideo::startAudioPlayback(double fromSeconds) {
    if (!audioEngine_ || audioClipId_ < 0) return;
    if (muted_) return;
    stopAudioPlayback();
    audioPlaybackId_ = audioEngine_->playClip(audioClipId_, static_cast<float>(volume_), false);
    if (audioPlaybackId_ < 0) return;
    audioEngine_->setPlaybackRate(audioPlaybackId_, static_cast<float>(playbackRate_));
    if (fromSeconds > 0.0) {
        // Clip is stored at the engine's sample rate (resampled at load).
        int frames = audioEngine_->getClipSampleCount(audioClipId_);
        int start = static_cast<int>(fromSeconds * audioEngine_->sampleRate());
        if (start < 0) start = 0;
        if (start >= frames) start = frames > 0 ? frames - 1 : 0;
        audioEngine_->setPlaybackRegion(audioPlaybackId_, start, frames);
    }
}

void ElVideo::stopAudioPlayback() {
    if (!audioEngine_ || audioPlaybackId_ < 0) return;
    audioEngine_->stopPlayback(audioPlaybackId_);
    audioPlaybackId_ = -1;
}

void ElVideo::play() {
    if (!pipeline_) return;
    pipeline_->play();
    // A/V sync: start audio from the current video clock position. For the
    // short-clip MVP both advance on independent clocks; drift over a few
    // seconds is imperceptible. Longer clips will pull the master clock from
    // the audio output (getPlaybackPosition()) when we add streaming audio.
    if (audioPlaybackId_ < 0) {
        startAudioPlayback(currentTime());
    } else if (audioEngine_) {
        audioEngine_->setPlaybackPlaying(audioPlaybackId_, true);
    }
}
void ElVideo::pause() {
    if (pipeline_) pipeline_->pause();
    if (audioEngine_ && audioPlaybackId_ >= 0) {
        audioEngine_->setPlaybackPlaying(audioPlaybackId_, false);
    }
}
bool ElVideo::isPlaying() const { return pipeline_ && pipeline_->isPlaying(); }

void ElVideo::seekTo(double seconds) {
    if (!pipeline_) return;
    auto ns = static_cast<bro::video::TimeNs>(seconds * 1e9);
    pipeline_->seekTo(ns);
    pipeline_->advanceTo(ns);
    // Seek can move playback away from the end; let ended fire again if the
    // stream is re-played past its tail, and force the next timeupdate.
    endedFired_ = false;
    lastTimeUpdateSec_ = -1.0;
    // Re-anchor audio at the new position if we were playing.
    if (pipeline_->isPlaying() && audioClipId_ >= 0) {
        startAudioPlayback(seconds);
    } else {
        stopAudioPlayback();
    }
}

double ElVideo::currentTime() const {
    return pipeline_ ? pipeline_->currentPts() / 1e9 : 0.0;
}

double ElVideo::duration() const {
    return pipeline_ ? pipeline_->durationNs() / 1e9 : 0.0;
}

bool ElVideo::isReady() const {
    return pipeline_ && pipeline_->hasFrame();
}

bool ElVideo::isEnded() const {
    return pipeline_ && pipeline_->isEnded();
}

void ElVideo::setVolume(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    if (volume_ == v) return;
    volume_ = v;
    applyAudioVolume();
}

void ElVideo::setMuted(bool m) {
    if (muted_ == m) return;
    muted_ = m;
    if (muted_) {
        stopAudioPlayback();
    } else if (pipeline_ && pipeline_->isPlaying() && audioClipId_ >= 0) {
        startAudioPlayback(currentTime());
    }
}

void ElVideo::setPlaybackRate(double r) {
    if (r <= 0.0) r = 1.0;
    playbackRate_ = r;
    if (pipeline_) pipeline_->setRate(r);
    if (audioEngine_ && audioPlaybackId_ >= 0) {
        audioEngine_->setPlaybackRate(audioPlaybackId_, static_cast<float>(r));
    }
}

void ElVideo::applyAudioVolume() {
    if (!audioEngine_ || audioPlaybackId_ < 0) return;
    audioEngine_->setPlaybackGain(audioPlaybackId_, static_cast<float>(volume_));
}

void ElVideo::getContentSize(float& w, float& h) {
    w = static_cast<float>(intrinsicWidth_);
    h = static_cast<float>(intrinsicHeight_);
}

void ElVideo::draw(render::Renderer* renderer,
                   dom::Element* elem,
                   const htmlayout::layout::LayoutBox& box,
                   float offsetX, float offsetY) {
    if (!renderer || !elem) return;

    const float x = box.contentRect.x + offsetX;
    const float y = box.contentRect.y + offsetY;
    const float w = box.contentRect.width;
    const float h = box.contentRect.height;

    if (!pipeline_) {
        renderer->fillRect(x, y, w, h, cfromColor8({0, 0, 0, 255}));
        return;
    }

    // Pull frames up to the current clock time. Paused state is tracked
    // by the pipeline's FileClock, which freezes nowNs() between
    // pause() and play().
    pipeline_->advance();
    // NOTE: pumpEvents() runs on the main thread (Engine::pumpVideoEvents)
    // — QuickJS is not thread-safe, and draw() executes on the raster
    // thread. Pipeline state read here (currentPts, hasFrame) is written by
    // this advance() call; main-thread pumpEvents reads it with a stale-
    // read tolerance (same discipline as the rest of the pipeline state).

    if (pipeline_->hasFrame() && !pipeline_->currentRgba().empty()) {
        renderer->drawPixelsRGBA(pipeline_->currentRgba().data(),
                                  pipeline_->frameWidth(),
                                  pipeline_->frameHeight(),
                                  pipeline_->frameWidth() * 4,
                                  x, y, w, h);
    } else {
        renderer->fillRect(x, y, w, h, cfromColor8({0, 0, 0, 255}));
    }
}

void ElVideo::pumpEvents() {
    if (!jsCtx_ || !elem_ || !pipeline_) return;

    // loadedmetadata fires once after a successful open(). HTMLMediaElement
    // fires loadedmetadata even before the first frame has been decoded, but
    // at this point we've already primed one frame so dimensions/duration are
    // known — consistent with readyState >= HAVE_METADATA.
    if (pendingLoadedMetadata_) {
        pendingLoadedMetadata_ = false;
        dom::Event evt("loadedmetadata", false, false);
        evt.setIsTrusted(true);
        js::dispatchDomEvent(jsCtx_, elem_, evt);
        // durationchange fires alongside loadedmetadata when duration first
        // becomes known. Also fires if duration changes later (e.g. on reload).
        const double dur = duration();
        if (dur != lastDurationSec_) {
            lastDurationSec_ = dur;
            dom::Event devt("durationchange", false, false);
            devt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, devt);
        }
        dom::Event canplay("canplay", false, false);
        canplay.setIsTrusted(true);
        js::dispatchDomEvent(jsCtx_, elem_, canplay);
    }

    // canplaythrough: bro predecodes audio into a single clip and demuxes
    // video from a local file, so once metadata + first frame are ready we
    // can assert the stream will play through without buffering.
    if (pendingCanPlayThrough_ && !pendingLoadedMetadata_ && pipeline_->hasFrame()) {
        pendingCanPlayThrough_ = false;
        dom::Event evt("canplaythrough", false, false);
        evt.setIsTrusted(true);
        js::dispatchDomEvent(jsCtx_, elem_, evt);
    }

    const double t = currentTime();
    const double dur = duration();

    // waiting / playing: while the clock advances but no decoded frame is
    // available, the element is "stalled at the edge of decoded data". Fire
    // 'waiting' when that happens, 'playing' when a frame appears again.
    if (pipeline_->isPlaying() && !pipeline_->isEnded()) {
        const bool hasFrame = pipeline_->hasFrame();
        if (!hasFrame && !waiting_) {
            waiting_ = true;
            dom::Event evt("waiting", false, false);
            evt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, evt);
        } else if (hasFrame && waiting_) {
            waiting_ = false;
            dom::Event evt("playing", false, false);
            evt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, evt);
        }
    } else if (waiting_ && !pipeline_->isPlaying()) {
        // Paused while waiting — drop the flag so a later play() can recover.
        waiting_ = false;
    }

    // timeupdate: throttle to kTimeUpdateIntervalSec of media time. Fires
    // while playing OR after a seek (seekTo resets lastTimeUpdateSec_).
    if (pipeline_->isPlaying() || lastTimeUpdateSec_ < 0.0) {
        if (lastTimeUpdateSec_ < 0.0 ||
            std::fabs(t - lastTimeUpdateSec_) >= kTimeUpdateIntervalSec) {
            lastTimeUpdateSec_ = t;
            dom::Event evt("timeupdate", false, false);
            evt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, evt);
        }
    }

    // ended: fire once when the demuxer has drained and we've decoded the
    // last frame. Gate on the pipeline's own EOS flag rather than comparing
    // t to duration — the last packet's pts typically falls short of the
    // container-reported duration by one frame's worth of time.
    (void)dur;
    if (!endedFired_ && pipeline_->isEnded() && pipeline_->hasFrame()) {
        endedFired_ = true;
        pipeline_->pause();
        stopAudioPlayback();
        // When loop is set, rewind and resume instead of firing ended — matches
        // HTMLMediaElement.loop behavior (no ended event while looping).
        if (loop_) {
            seekTo(0.0);
            pipeline_->play();
            if (audioClipId_ >= 0 && !muted_) {
                startAudioPlayback(0.0);
            }
            endedFired_ = false;
        } else {
            dom::Event evt("ended", false, false);
            evt.setIsTrusted(true);
            js::dispatchDomEvent(jsCtx_, elem_, evt);
        }
    }
}

} // namespace bro::layout

#else  // !BRO_WITH_VIDEO

// ── Inert stub — video-less build (no libvpx/webm/Opus, no vcpkg). <video>
// elements still exist and their HTMLMediaElement JS surface stays callable
// (element_bindings.cpp is core), but nothing decodes: the element paints a
// black content box and all media state reads as empty/paused. pipeline_ is
// always null and no video/* headers are pulled in.
namespace bro::layout {

using bromath::cfromColor8;

ElVideo::ElVideo(render::Renderer* renderer) : renderer_(renderer) {}
ElVideo::~ElVideo() = default;  // pipeline_ is always null in this build

bool   ElVideo::load(const std::string&) { return false; }
void   ElVideo::openAudioTrack(const std::string&) {}
void   ElVideo::startAudioPlayback(double) {}
void   ElVideo::stopAudioPlayback() {}
void   ElVideo::play() {}
void   ElVideo::pause() {}
bool   ElVideo::isPlaying() const { return false; }
void   ElVideo::seekTo(double) {}
double ElVideo::currentTime() const { return 0.0; }
double ElVideo::duration() const { return 0.0; }
bool   ElVideo::isReady() const { return false; }
bool   ElVideo::isEnded() const { return false; }
void   ElVideo::applyAudioVolume() {}

void ElVideo::setVolume(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    volume_ = v;  // keep IDL getter coherent even with no audio track
}
void ElVideo::setMuted(bool m) { muted_ = m; }
void ElVideo::setPlaybackRate(double r) {
    if (r <= 0.0) r = 1.0;
    playbackRate_ = r;
}

void ElVideo::getContentSize(float& w, float& h) {
    w = static_cast<float>(intrinsicWidth_);
    h = static_cast<float>(intrinsicHeight_);
}

void ElVideo::draw(render::Renderer* renderer, dom::Element* elem,
                   const htmlayout::layout::LayoutBox& box,
                   float offsetX, float offsetY) {
    if (!renderer || !elem) return;
    renderer->fillRect(box.contentRect.x + offsetX, box.contentRect.y + offsetY,
                       box.contentRect.width, box.contentRect.height,
                       cfromColor8({0, 0, 0, 255}));
}

void ElVideo::pumpEvents() {}

} // namespace bro::layout

#endif  // BRO_WITH_VIDEO
