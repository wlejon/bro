#pragma once

#include "layout/box.h"
#include "render/renderer.h"

#include <memory>
#include <string>

extern "C" { typedef struct JSContext JSContext; }

namespace bro::dom { class Element; }
namespace bro::video { class VideoPipeline; }
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

    void draw(render::Renderer* renderer,
              dom::Element* elem,
              const htmlayout::layout::LayoutBox& box,
              float offsetX, float offsetY);

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

    bro::video::VideoPipeline* pipeline() const { return pipeline_.get(); }

    // Dispatch any pending HTMLMediaElement events on the element's JS
    // listeners. MUST be called on the main thread — uses the JSContext
    // passed via setJsContext(). draw() runs on the raster thread, so the
    // engine pumps events from its main loop instead.
    void pumpEvents();

private:
    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    std::unique_ptr<bro::video::VideoPipeline> pipeline_;

    int intrinsicWidth_ = 300;
    int intrinsicHeight_ = 150;

    // Event-lifecycle bookkeeping. ElVideo fires media events during draw()
    // (on the main thread, with a known JSContext). Fields here latch what
    // still needs to be dispatched on the next pump.
    JSContext* jsCtx_ = nullptr;
    bool pendingLoadedMetadata_ = false;
    bool endedFired_ = false;
    double lastTimeUpdateSec_ = -1.0;

    // Audio is predecoded at load() into a single broaudio clip and played
    // back via a clip playback instance. The first iteration buffers the
    // whole audio track; streaming audio decode arrives with the decode
    // thread. audioClipId_ = -1 when the source has no audio track.
    broaudio::Engine* audioEngine_ = nullptr;
    int audioClipId_ = -1;
    int audioPlaybackId_ = -1;
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
