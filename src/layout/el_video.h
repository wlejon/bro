#pragma once

#include "layout/box.h"
#include "render/renderer.h"

#include <memory>
#include <string>

extern "C" { typedef struct JSContext JSContext; }

namespace bro::dom { class Element; }
namespace bro::video { class VideoPipeline; class MediaSource; class AudioDecoder; }
namespace broaudio { class Engine; }

namespace bro::layout {

// Replaced-element controller for <video>. Owns a VideoPipeline and
// renders its current decoded frame into the element's content box on
// each draw. Attribute handling (src, autoplay, loop, muted, controls)
// is triggered by the JS binding via load(), play(), pause(), seek().
class ElVideo {
public:
    explicit ElVideo(render::Renderer* renderer);
    ~ElVideo();

    /// `objectFit` is the CSS object-fit keyword: "fill" (default, stretch),
    /// "contain", "cover", "none" or "scale-down".
    void draw(render::Renderer* renderer,
              dom::Element* elem,
              const htmlayout::layout::LayoutBox& box,
              float offsetX, float offsetY,
              const std::string& objectFit = "fill");

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    // Set once by the engine so media events (loadedmetadata, timeupdate,
    // ended) can be dispatched to JS listeners. Null in contexts without a
    // JS runtime — events are silently dropped in that case.
    void setJsContext(JSContext* ctx) { jsCtx_ = ctx; }

    // Set once by the engine so decoded audio can be routed through the
    // shared broaudio graph. Null in contexts without an audio engine.
    void setAudioEngine(broaudio::Engine* eng) { audioEngine_ = eng; }

    // Open a WebM file; returns true on success. Does not auto-play.
    bool load(const std::string& path);

    void play();
    void pause();
    bool isPlaying() const;
    void seekTo(double seconds);
    double currentTime() const;
    double duration() const;
    bool isReady() const;   // have a decoded frame and tracks
    bool isEnded() const;   // pipeline has drained and decoded last frame
    bool hasPipeline() const { return pipeline_ != nullptr; }

    // Resolved URL of the currently-loaded resource. Empty when no resource
    // has been selected. Matches HTMLMediaElement.currentSrc.
    const std::string& currentSrc() const { return currentSrc_; }

    // Media element state (not backed by pipeline — tracked here so IDL
    // getters/setters are coherent and media events can fire when they change).
    double volume() const { return volume_; }
    void setVolume(double v);
    bool muted() const { return muted_; }
    void setMuted(bool m);
    double playbackRate() const { return playbackRate_; }
    void setPlaybackRate(double r);
    double defaultPlaybackRate() const { return defaultPlaybackRate_; }
    void setDefaultPlaybackRate(double r) { defaultPlaybackRate_ = r; }
    bool loopEnabled() const { return loop_; }
    void setLoopEnabled(bool l) { loop_ = l; }

    void getContentSize(float& w, float& h);

    int videoWidth() const { return intrinsicWidth_; }
    int videoHeight() const { return intrinsicHeight_; }

    bro::video::VideoPipeline* pipeline() const { return pipeline_; }

    // Dispatch any pending HTMLMediaElement events on the element's JS
    // listeners. MUST be called on the main thread — uses the JSContext
    // passed via setJsContext(). draw() runs on the raster thread, so the
    // engine pumps events from its main loop instead.
    void pumpEvents();

    // Pull the pipeline up to its clock without drawing anything.
    //
    // Normally draw() does this, because the frame it decodes is the frame it
    // is about to present. Headless has no raster thread and only renders when
    // a script asks for a screenshot, so without this a playing <video> sits
    // at 0:00 through any amount of flush() — the clock runs but nothing ever
    // consumes it. Safe there precisely because nothing else is touching the
    // pipeline concurrently; the windowed engine must NOT call it.
    void advancePipeline();

private:
    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    // Owning raw pointer (not unique_ptr) so ElVideo's destructor does not
    // need VideoPipeline complete — that would force the vcpkg-backed video/*
    // headers into core layout. new/delete happen only inside #if BRO_WITH_VIDEO
    // in el_video.cpp; in a video-less build this stays null and the inert stub
    // impl never touches it.
    bro::video::VideoPipeline* pipeline_ = nullptr;

    int intrinsicWidth_ = 300;
    int intrinsicHeight_ = 150;

    // Event-lifecycle bookkeeping. ElVideo fires media events during draw()
    // (on the main thread, with a known JSContext). Fields here latch what
    // still needs to be dispatched on the next pump.
    JSContext* jsCtx_ = nullptr;
    bool pendingLoadedMetadata_ = false;
    bool pendingCanPlayThrough_ = false;
    bool endedFired_ = false;
    bool waiting_ = false;
    double lastTimeUpdateSec_ = -1.0;
    std::string currentSrc_;

    // Audio takes one of two routes, decided at load() by whether the
    // backend's decoder can deliver PCM at the audio engine's sample rate
    // (AudioDecoder::setOutputFormat).
    //
    //  - STREAMING (audioStreamId_ >= 0): a live broaudio PCM ring topped up
    //    from pumpEvents() as it drains. The only workable route for real
    //    media — a two-hour film's audio track is gigabytes decoded, and
    //    decoding it up front would stall load() for a minute.
    //  - PREDECODED (audioClipId_ >= 0): the whole track decoded into one
    //    clip at load(). Used when the decoder can't resample, which today
    //    means bro's built-in Opus path (libopus decodes only to its own
    //    rates, and bro's resampler is one-shot). Fine for the short clips
    //    that path serves.
    //
    // Both -1 when the source has no audio track.
    broaudio::Engine* audioEngine_ = nullptr;
    int audioClipId_ = -1;
    int audioPlaybackId_ = -1;

    // Streaming route. Its own demuxer + decoder: VideoPipeline's source is
    // filtered down to the video track, and interleaving two consumers on one
    // demuxer would need a packet queue between them.
    bro::video::MediaSource* audioSource_ = nullptr;
    bro::video::AudioDecoder* audioStreamDec_ = nullptr;
    uint32_t audioSourceTrackId_ = 0;
    int audioStreamId_ = -1;
    int audioStreamChannels_ = 0;
    int audioStreamRate_ = 0;
    bool audioSourceEnded_ = false;

    bool openStreamingAudio(const std::string& resolvedPath);
    void closeStreamingAudio();
    void pumpStreamingAudio();
    void restartStreamingAudio(double fromSeconds);

    void openAudioTrack(const std::string& resolvedPath);
    void startAudioPlayback(double fromSeconds);
    void stopAudioPlayback();
    void applyAudioVolume();

    // HTMLMediaElement IDL state. volume/muted gate audio playback; others
    // are currently informational (playbackRate plumbing into the pipeline
    // clock is deferred until it's exercised by an app).
    double volume_ = 1.0;
    bool muted_ = false;
    double playbackRate_ = 1.0;
    double defaultPlaybackRate_ = 1.0;
    bool loop_ = false;
    double lastDurationSec_ = -1.0;
};

} // namespace bro::layout
