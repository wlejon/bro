#include "layout/el_video.h"

#include "render/renderer.h"

#include <cmath>

#if BRO_WITH_VIDEO

#include "broaudio/engine.h"
#include "broaudio/dsp/resampler.h"
#include "dom/element.h"
#include "dom/event.h"
#include "js/event_dispatch.h"
#include "util/log.h"
#include "video/audio_decoder.h"
#include "video/media_backend.h"
#include "video/video_pipeline.h"

namespace bro::layout {

using bromath::cfromColor8;

// HTMLMediaElement spec allows 4–66 Hz. 250 ms of media time is well within
// that range and matches Chromium's low-rate path.
static constexpr double kTimeUpdateIntervalSec = 0.25;


// How much decoded audio to keep queued ahead of the mixer. Half a second
// absorbs a slow frame or a GC pause without the ring running dry, and is
// small enough that a seek doesn't have to throw much away.
static constexpr double kAudioBufferSeconds = 0.5;

// Streaming audio mixes at most this many channels. broaudio's live PCM
// streams are mono/stereo, so a 5.1 track is downmixed by the decoder's
// resampler, which folds centre and surrounds in at the right levels rather
// than dropping them.
static constexpr uint32_t kMaxStreamChannels = 2;

ElVideo::ElVideo(render::Renderer* renderer) : renderer_(renderer) {}
ElVideo::~ElVideo() {
    closeStreamingAudio();
    delete pipeline_;
}

bool ElVideo::load(const std::string& path) {
    auto* p = new bro::video::VideoPipeline();
    const std::string resolved = elem_ ? elem_->resolveUrl(path) : path;
    if (!p->open(resolved)) { delete p; return false; }
    // Setting .src twice used to leak the previous pipeline and leave the old
    // audio playing under the new video.
    closeStreamingAudio();
    stopAudioPlayback();
    audioClipId_ = -1;
    delete pipeline_;
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

    // Audio runs on its own demuxer: VideoPipeline's source pumps video only
    // and drops audio packets, so the file is opened a second time. Prefer
    // streaming; fall back to predecoding when the decoder can't resample.
    if (audioEngine_ && pipeline_->audioDecoder()) {
        if (!openStreamingAudio(resolved)) openAudioTrack(resolved);
    }
    return true;
}

// ── Streaming audio ────────────────────────────────────────────────────────

bool ElVideo::openStreamingAudio(const std::string& resolvedPath) {
    closeStreamingAudio();
    if (!audioEngine_) return false;

    std::unique_ptr<bro::video::MediaSource> source;
    const bro::video::MediaBackend* backend = nullptr;
    for (const auto& be : bro::video::mediaBackends()) {
        if (!be.open) continue;
        source = be.open(resolvedPath);
        if (source) { backend = &be; break; }
    }
    if (!source || !backend || !backend->makeAudioDecoder) return false;

    const uint32_t engineRate = static_cast<uint32_t>(audioEngine_->sampleRate());
    std::unique_ptr<bro::video::AudioDecoder> decoder;
    uint32_t trackId = 0, channels = 0;
    for (const auto& t : source->tracks()) {
        if (t.kind != bro::video::TrackKind::Audio) continue;
        auto dec = backend->makeAudioDecoder(t);
        if (!dec) continue;
        channels = t.channels > kMaxStreamChannels ? kMaxStreamChannels : t.channels;
        if (channels == 0) continue;
        // The whole decision: a decoder that can hand back engine-rate PCM
        // chunk by chunk can stream. One that can't would need a resampler
        // carrying state across calls, which bro doesn't have.
        if (!dec->setOutputFormat(engineRate, channels)) continue;
        decoder = std::move(dec);
        trackId = t.id;
        break;
    }
    if (!decoder || trackId == 0) return false;

    // Now that we know which track we want, stop the demuxer delivering the
    // video packets we would only throw away.
    source->setActiveTracks({trackId});

    const int ringFrames = static_cast<int>(engineRate * (kAudioBufferSeconds * 2));
    int streamId = audioEngine_->createStream(static_cast<int>(channels), ringFrames);
    if (streamId < 0) return false;
    // Created running; hold it until play(). Nothing has been pushed yet, so
    // leaving it live would just accumulate underruns.
    audioEngine_->setPlaybackPlaying(streamId, false);
    audioEngine_->setPlaybackGain(streamId, muted_ ? 0.0f : static_cast<float>(volume_));

    audioSource_ = source.release();
    audioStreamDec_ = decoder.release();
    audioSourceTrackId_ = trackId;
    audioStreamId_ = streamId;
    audioStreamChannels_ = static_cast<int>(channels);
    audioStreamRate_ = static_cast<int>(engineRate);
    audioSourceEnded_ = false;

    // Prime the ring so the first play() starts on sound, not on silence.
    pumpStreamingAudio();
    LOG_INFO("video: audio streaming (%d ch @ %d Hz, %.0f ms ring)",
             audioStreamChannels_, audioStreamRate_, kAudioBufferSeconds * 2000.0);
    return true;
}

void ElVideo::closeStreamingAudio() {
    if (audioEngine_ && audioStreamId_ >= 0) audioEngine_->closeStream(audioStreamId_);
    audioStreamId_ = -1;
    delete audioStreamDec_;
    audioStreamDec_ = nullptr;
    delete audioSource_;
    audioSource_ = nullptr;
    audioSourceTrackId_ = 0;
    audioStreamChannels_ = 0;
    audioStreamRate_ = 0;
    audioSourceEnded_ = false;
}

// Top the ring back up to kAudioBufferSeconds. Called once per frame from
// pumpEvents() — main thread, so decode never runs on the audio thread.
void ElVideo::pumpStreamingAudio() {
    if (!audioEngine_ || audioStreamId_ < 0 || !audioSource_ || !audioStreamDec_) return;
    if (audioSourceEnded_) return;

    auto stats = audioEngine_->getStreamStats(audioStreamId_);
    if (!stats.valid) return;

    const int64_t target = static_cast<int64_t>(audioStreamRate_ * kAudioBufferSeconds);
    int64_t buffered = static_cast<int64_t>(stats.bufferedFrames);

    bro::video::MediaPacket pkt;
    bro::video::AudioFrame frame;
    while (buffered < target) {
        if (!audioSource_->readPacket(pkt)) { audioSourceEnded_ = true; break; }
        if (pkt.trackId != audioSourceTrackId_) continue;
        if (!audioStreamDec_->decode(pkt, frame) || frame.samples.empty()) continue;
        const int pushed = audioEngine_->pushStreamSamples(
            audioStreamId_, frame.samples.data(), static_cast<int>(frame.samples.size()));
        if (pushed <= 0) break;   // ring full: try again next frame
        buffered += pushed;
    }
}

// Seek: drop everything queued and re-anchor the decoder at the new position.
// The ring has no seek of its own, so it is torn down and rebuilt — cheaper
// than it sounds, and the alternative is half a second of stale audio.
void ElVideo::restartStreamingAudio(double fromSeconds) {
    if (!audioEngine_ || audioStreamId_ < 0 || !audioSource_ || !audioStreamDec_) return;

    const bool wasPlaying = pipeline_ && pipeline_->isPlaying();
    audioEngine_->closeStream(audioStreamId_);
    audioStreamId_ = -1;

    audioSource_->seekTo(static_cast<bro::video::TimeNs>(fromSeconds * 1e9));
    audioStreamDec_->flush();
    audioSourceEnded_ = false;

    const int ringFrames = static_cast<int>(audioStreamRate_ * (kAudioBufferSeconds * 2));
    audioStreamId_ = audioEngine_->createStream(audioStreamChannels_, ringFrames);
    if (audioStreamId_ < 0) return;
    audioEngine_->setPlaybackPlaying(audioStreamId_, false);
    audioEngine_->setPlaybackGain(audioStreamId_, muted_ ? 0.0f : static_cast<float>(volume_));
    audioEngine_->setPlaybackRate(audioStreamId_, static_cast<float>(playbackRate_));

    pumpStreamingAudio();
    if (wasPlaying) audioEngine_->setPlaybackPlaying(audioStreamId_, true);
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
    LOG_INFO("video: audio predecoded (%u ch @ %u Hz, %.1f s, %.1f MB) — this "
             "decoder cannot resample, so the whole track is resident",
             channels, sampleRate, numFrames / double(engineRate),
             pcm.size() * sizeof(float) / (1024.0 * 1024.0));
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
    if (!audioEngine_) return;
    if (audioStreamId_ >= 0) {
        // A live stream is halted, not destroyed: play() after ended has to
        // be able to resume it, and a seek rebuilds it anyway.
        audioEngine_->setPlaybackPlaying(audioStreamId_, false);
        return;
    }
    if (audioPlaybackId_ < 0) return;
    audioEngine_->stopPlayback(audioPlaybackId_);
    audioPlaybackId_ = -1;
}

void ElVideo::play() {
    if (!pipeline_) return;
    pipeline_->play();
    if (audioStreamId_ >= 0 && audioEngine_) {
        // Refill first: starting a drained ring emits silence and counts it
        // as underrun, which is exactly the "first second is missing" bug.
        pumpStreamingAudio();
        audioEngine_->setPlaybackPlaying(audioStreamId_, true);
        return;
    }
    // Predecoded route. Both clocks advance off real time independently;
    // drift over the short clips this route serves is imperceptible.
    if (audioPlaybackId_ < 0) {
        startAudioPlayback(currentTime());
    } else if (audioEngine_) {
        audioEngine_->setPlaybackPlaying(audioPlaybackId_, true);
    }
}
void ElVideo::pause() {
    if (pipeline_) pipeline_->pause();
    if (!audioEngine_) return;
    if (audioStreamId_ >= 0) {
        audioEngine_->setPlaybackPlaying(audioStreamId_, false);
    } else if (audioPlaybackId_ >= 0) {
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
    // Re-anchor audio at the new position.
    if (audioStreamId_ >= 0) {
        restartStreamingAudio(seconds);
    } else if (pipeline_->isPlaying() && audioClipId_ >= 0) {
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
    if (audioStreamId_ >= 0) {
        // Gain, not stop: the ring must keep draining in step with the video
        // clock so unmuting lands at the right place instead of half a
        // second behind.
        applyAudioVolume();
        return;
    }
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
    if (!audioEngine_) return;
    if (audioStreamId_ >= 0) {
        audioEngine_->setPlaybackRate(audioStreamId_, static_cast<float>(r));
    } else if (audioPlaybackId_ >= 0) {
        audioEngine_->setPlaybackRate(audioPlaybackId_, static_cast<float>(r));
    }
}

void ElVideo::applyAudioVolume() {
    if (!audioEngine_) return;
    const float gain = muted_ ? 0.0f : static_cast<float>(volume_);
    if (audioStreamId_ >= 0) {
        audioEngine_->setPlaybackGain(audioStreamId_, gain);
    } else if (audioPlaybackId_ >= 0) {
        audioEngine_->setPlaybackGain(audioPlaybackId_, static_cast<float>(volume_));
    }
}

void ElVideo::getContentSize(float& w, float& h) {
    w = static_cast<float>(intrinsicWidth_);
    h = static_cast<float>(intrinsicHeight_);
}

void ElVideo::draw(render::Renderer* renderer,
                   dom::Element* elem,
                   const htmlayout::layout::LayoutBox& box,
                   float offsetX, float offsetY,
                   const std::string& objectFit) {
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
        const float fw = static_cast<float>(pipeline_->frameWidth());
        const float fh = static_cast<float>(pipeline_->frameHeight());

        float dx = x, dy = y, dw = w, dh = h;
        bool needClip = false;
        if (objectFit != "fill" && fw > 0 && fh > 0) {
            float scale = 1.0f;
            if (objectFit == "contain")          scale = std::min(w / fw, h / fh);
            else if (objectFit == "cover")       scale = std::max(w / fw, h / fh);
            else if (objectFit == "none")        scale = 1.0f;
            else if (objectFit == "scale-down")  scale = std::min(1.0f, std::min(w / fw, h / fh));
            dw = fw * scale;
            dh = fh * scale;
            // object-position is not plumbed through yet; centre, which is
            // its default and what a viewport wants anyway.
            dx = x + (w - dw) * 0.5f;
            dy = y + (h - dh) * 0.5f;
            // Letterbox bars, and the backdrop for a cover crop.
            renderer->fillRect(x, y, w, h, cfromColor8({0, 0, 0, 255}));
            // cover and none can spill outside the content box.
            needClip = (dw > w + 0.5f) || (dh > h + 0.5f) ||
                       dx < x - 0.5f || dy < y - 0.5f;
        }

        if (needClip) { renderer->save(); renderer->setClip(x, y, w, h); }
        renderer->drawPixelsRGBA(pipeline_->currentRgba().data(),
                                  pipeline_->frameWidth(),
                                  pipeline_->frameHeight(),
                                  pipeline_->frameWidth() * 4,
                                  dx, dy, dw, dh);
        if (needClip) renderer->restore();
    } else {
        renderer->fillRect(x, y, w, h, cfromColor8({0, 0, 0, 255}));
    }
}

void ElVideo::advancePipeline() {
    if (pipeline_) pipeline_->advance();
}

void ElVideo::pumpEvents() {
    if (!pipeline_) return;
    // Keep the audio ring ahead of the mixer. Deliberately before the jsCtx_
    // guard: a document with no JS listeners still has to make sound.
    pumpStreamingAudio();
    if (!jsCtx_ || !elem_) return;

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
            if (audioStreamId_ >= 0 && audioEngine_) {
                audioEngine_->setPlaybackPlaying(audioStreamId_, true);
            } else if (audioClipId_ >= 0 && !muted_) {
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
bool   ElVideo::openStreamingAudio(const std::string&) { return false; }
void   ElVideo::closeStreamingAudio() {}
void   ElVideo::pumpStreamingAudio() {}
void   ElVideo::restartStreamingAudio(double) {}
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
void   ElVideo::advancePipeline() {}
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
                   float offsetX, float offsetY,
                   const std::string& /*objectFit*/) {
    if (!renderer || !elem) return;
    renderer->fillRect(box.contentRect.x + offsetX, box.contentRect.y + offsetY,
                       box.contentRect.width, box.contentRect.height,
                       cfromColor8({0, 0, 0, 255}));
}

void ElVideo::pumpEvents() {}

} // namespace bro::layout

#endif  // BRO_WITH_VIDEO
