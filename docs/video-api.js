// =============================================================================
// bro Video API — <video> playback + WebM/GIF encoders
// =============================================================================
//
// Two surfaces, both gated behind BRO_WITH_VIDEO (vcpkg: libvpx/libwebm/Opus):
//
//   1. <video> element playback — an HTMLMediaElement subset backed by the
//      engine's WebM pipeline (VP9 video + Opus audio, local files). See the
//      "<video> playback" section at the bottom of this file.
//   2. VideoEncoder / GifEncoder — RGBA frames in, .webm / .gif file out.
//
// In a video-less build the encoders are absent (feature-detect
// `typeof VideoEncoder`); <video> elements still exist with the full JS
// surface callable, but nothing decodes — the element paints a black box,
// load() fails, duration stays NaN.
//
// VideoEncoder writes a WebM file with a VP9 video track and an optional
// Opus audio track. Frames are RGBA in (libvpx software encode); audio is
// caller-provided interleaved float PCM (libopus). The audio track is only
// created when audioSampleRate is set in the constructor config.
//
// GifEncoder writes an animated GIF89a with a per-frame 256-color palette
// (median-cut quantization). Suitable for sprite/animation export and for
// quick sharing of pixel art; for natural-image content prefer WebM.
//
// Hardware encode is intentionally not exposed: VP9 hardware support is
// sparse (no NVENC, no D3D12 Video Encode), so a HW path would require
// a different codec. When that becomes a bottleneck, expect a sibling
// class (H264Encoder / AV1Encoder) sharing the same shape.
//
// Available in both windowed and headless. Output paths are taken as-is
// (resolved against the engine's cwd), matching how `screenshot()` and
// `screenshotCanvas()` treat paths — no implicit basePath prepending.
// =============================================================================


// -----------------------------------------------------------------------------
// new VideoEncoder({ path, width, height, fps?, fpsDen?, bitrateKbps?,
//                    keyframeIntervalSec?, quality?, threads? })
// -----------------------------------------------------------------------------
//
// Required:
//   path     — output .webm file path (string).
//   width,
//   height   — frame size in pixels. Both must be even (4:2:0 chroma).
//
// Optional:
//   fps           — integer frames per second (default 30).
//   fpsDen        — fps denominator if you need a non-integer rate
//                   (e.g. fps=24000, fpsDen=1001 for 23.976). Default 1.
//   bitrateKbps   — VBR target. Default auto: ~0.07 bits/pixel/frame,
//                   clamped to [200, 8000].
//   keyframeIntervalSec — max gap between keyframes (default 2). The
//                   encoder may insert extra keyframes on top of this.
//   quality       — 'realtime' | 'good' | 'best' (default 'good').
//                   Maps to libvpx VPX_DL_REALTIME / GOOD / BEST and
//                   tunes cpu-used (7 / 1 / 0).
//   threads       — encoder threads (default 1). Bump for >720p.
//   audioSampleRate  — 8000 / 12000 / 16000 / 24000 / 48000. Set to enable
//                      the Opus audio track; 0 (default) = no audio.
//   audioChannels    — 1 (mono) or 2 (stereo). Default 2.
//   audioBitrateKbps — Opus VBR target. Default 96.
//
// Throws on invalid config or file open failure. Constructor returns an
// open encoder ready for frames.

const enc = new VideoEncoder({
    path: 'output/clip.webm',
    width: 256,
    height: 192,
    fps: 30,
    quality: 'realtime',
});


// -----------------------------------------------------------------------------
// enc.addFrameRGBA(uint8Array [, stride])
// -----------------------------------------------------------------------------
//
// Push one RGBA frame, top-down, 4 bytes per pixel. The buffer must hold at
// least stride*height bytes. `stride` defaults to width*4. Returns true; on
// encoder failure throws with the libvpx error string.
//
// The encoder copies pixels into its own YUV plane, so the caller can reuse
// the same Uint8Array for the next frame.

const frame = new Uint8Array(256 * 192 * 4);
for (let i = 0; i < 60; i++) {
    drawSomethingInto(frame);
    enc.addFrameRGBA(frame);
}


// -----------------------------------------------------------------------------
// enc.addCanvasFrame(canvasElement)
// -----------------------------------------------------------------------------
//
// Snapshot the canvas's underlying Skia surface (preserves alpha, no
// framebuffer flatten) and encode it. Same pixel-source path as the
// headless screenshotCanvas helper.
//
// The canvas dimensions must match the encoder dimensions — there's no
// implicit resize. RangeError if they differ.

const sheet = document.querySelector('#sheet');
sheet.width = 256;
sheet.height = 192;
const ctx = sheet.getContext('2d');
for (let i = 0; i < 60; i++) {
    drawFrame(ctx, i);
    flush();                         // ensure the surface picks up new commands
    enc.addCanvasFrame(sheet);
}


// -----------------------------------------------------------------------------
// enc.addAudioFramesPCM(float32Array)
// -----------------------------------------------------------------------------
//
// Push interleaved float PCM at the configured sample rate / channel count.
// Length must be a multiple of audioChannels. Samples are buffered and
// encoded into 20 ms Opus packets; any trailing partial chunk is zero-padded
// inside finish(). Throws if the encoder was not configured with audio.
//
// Mono example (1 second of 440 Hz sine at 48 kHz):
//   const sr = 48000;
//   const enc = new VideoEncoder({ path: 'out.webm', width: 256, height: 256,
//                                   audioSampleRate: sr, audioChannels: 1 });
//   const samples = new Float32Array(sr);
//   for (let i = 0; i < sr; i++) samples[i] = 0.2 * Math.sin(2*Math.PI*440*i/sr);
//   enc.addAudioFramesPCM(samples);
//
// Stereo input is interleaved L,R,L,R,... — same shape as Web Audio's
// channel-interleaved buffers.


// -----------------------------------------------------------------------------
// enc.finish() → boolean
// -----------------------------------------------------------------------------
//
// Flush remaining frames out of the encoder and close the WebM trailer.
// Idempotent — subsequent calls return true. The encoder also calls finish()
// when garbage-collected, but the file isn't fully written until you do
// (or the encoder is collected and finalized).

enc.finish();


// -----------------------------------------------------------------------------
// Read-only properties
// -----------------------------------------------------------------------------
//
// enc.width, enc.height       — configured frame size
// enc.framesWritten           — number of compressed packets muxed so far
// enc.lastError               — last libvpx / muxer error string (empty if ok)


// -----------------------------------------------------------------------------
// Recipe: record a procedural animation in headless
// -----------------------------------------------------------------------------
//
// Combine with advanceTime() to drive a virtual clock and render frame-by-frame.

const fps = 24;
const enc2 = new VideoEncoder({
    path: 'output/anim.webm', width: 320, height: 240, fps,
});
for (let i = 0; i < fps * 4; i++) {
    advanceTime(1000 / fps);
    flush();
    enc2.addCanvasFrame(document.querySelector('#stage'));
}
enc2.finish();


// =============================================================================
// GifEncoder
// =============================================================================
//
// new GifEncoder({ path, width, height, fps?, delayCs?, paletteBits?, loopCount? })
//
//   path          — output .gif file path.
//   width, height — frame size in pixels (no even-size requirement).
//   fps           — frames per second (default 25). Converted internally to
//                   delayCs = round(100 / fps).
//   delayCs       — frame delay in centiseconds (1/100 sec). Used if fps
//                   isn't set. Default 4 (≈25 fps).
//   paletteBits   — 1..8 (default 8 = 256 colors per frame).
//   loopCount     — 0 = loop forever (default), 1 = play once,
//                   N = repeat N times.
//
// Pixels with alpha < 128 become the GIF transparent index; partial alpha
// is treated as opaque. Each frame carries its own quantized local palette.

const gif = new GifEncoder({
    path: 'output/clip.gif',
    width: 64, height: 64,
    fps: 12,
});
for (let i = 0; i < 24; i++) {
    drawSomethingInto(frame);
    gif.addFrameRGBA(frame);
}
gif.finish();

// Per-frame delay override:
gif.setNextFrameDelayCs(50);   // hold the next frame for 0.5 seconds
gif.addCanvasFrame(canvas);

// Same canvas-snapshot path as VideoEncoder — gif and webm encoders are
// interchangeable from the addFrame side.


// =============================================================================
// <video> playback — HTMLMediaElement subset
// =============================================================================
//
// A plain HTML <video> element plays local WebM files (VP9 video, Opus
// audio) through the engine's demux → decode → present pipeline. The decoded
// frame renders into the element's CSS content box each frame; audio routes
// through the shared broaudio engine (the same output AudioContext uses).
//
//   <video id="clip" src="assets/intro.webm"></video>
//
// Sources: local files only, resolved like other element URLs (relative to
// the document base; asset mounts like /lib and /system work). Container
// support is exactly WebM/VP9(+VP8)/Opus — canPlayType() answers honestly:
//
//   v.canPlayType('video/webm; codecs="vp9,opus"')  // "probably"
//   v.canPlayType('video/mp4')                      // "" (unsupported)
//
// A src attribute present at parse time loads immediately (metadata + first
// frame are primed synchronously, so videoWidth/duration are readable right
// after layout). Assigning `v.src = url` — or calling v.load() with a src
// attribute set — (re)loads the same way. Loading does NOT auto-play.
//
// Clock behavior (matters for testing): playback advances on the host wall
// clock, not the engine's virtual clock — headless advanceTime() does not
// move video time (use wallSleep), and bro.time pause/timescale do not
// affect a playing video.

const v = document.getElementById('clip');

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------
//
// v.play() → Promise<void>
//   Starts (or resumes) playback from currentTime and starts the audio
//   track alongside it. Fires 'play' when transitioning from paused.
//   The returned promise is already resolved (no autoplay policy to wait
//   on) — `await v.play()` and fire-and-forget both work.
//
// v.pause()
//   Freezes the pipeline clock and pauses audio. Fires 'pause' when
//   transitioning from playing.
//
// v.load()
//   Re-opens the resource named by the src attribute. No-op without src.
//
// v.canPlayType(mimeType) → "probably" | ""
//   "probably" for webm / vp8 / vp9 / opus / ogg-opus types, "" otherwise
//   (no "maybe" tier — support is known exactly).

v.play();     // returns an already-resolved Promise — awaiting is optional
v.pause();

// ---------------------------------------------------------------------------
// Time and seeking
// ---------------------------------------------------------------------------
//
// v.currentTime          — playback position in seconds. Assigning seeks
//                          SYNCHRONOUSLY: the pipeline decodes up to the
//                          target before the setter returns, firing
//                          'seeking' → 'seeked' → 'timeupdate' in one go.
//                          Because seeks complete inline, v.seeking always
//                          reads false.
// v.duration             — container duration in seconds; NaN until a
//                          resource is loaded.
// v.paused, v.ended      — booleans. Seeking away from the end re-arms
//                          'ended' so it can fire again.
// v.buffered, v.seekable — TimeRanges-shaped objects ({length, start(i),
//                          end(i)}) covering one run [0, duration]: local
//                          files decode sequentially with no gaps.
// v.played               — same shape, [0, currentTime] once playback has
//                          advanced past 0.

v.currentTime = 2.5;                    // seek — synchronous
console.log(v.currentTime, '/', v.duration);

// ---------------------------------------------------------------------------
// Audio: volume / muted / playbackRate
// ---------------------------------------------------------------------------
//
// v.volume       — 0..1 gain on the element's audio track (clamped).
//                  Fires 'volumechange' when the value actually moves.
// v.muted        — live mute. Fires 'volumechange' on change. Initialized
//                  from the `muted` content attribute at load time.
// v.defaultMuted — reflects the `muted` attribute itself.
// v.playbackRate — playback speed (> 0; invalid values reset to 1). Drives
//                  both the video clock and the audio playback rate (audio
//                  pitch-shifts — no time-stretch). Fires 'ratechange'.
// v.defaultPlaybackRate — stored/reflected only; not applied automatically.

v.volume = 0.5;
v.muted = true;
v.playbackRate = 2.0;

// ---------------------------------------------------------------------------
// Metadata and state
// ---------------------------------------------------------------------------
//
// v.videoWidth, v.videoHeight — intrinsic frame size (300×150 defaults
//                               before metadata, like the spec's replaced-
//                               element fallback).
// v.currentSrc     — resolved URL of the loaded resource ("" before load).
// v.readyState     — 0 HAVE_NOTHING (no pipeline), 1 HAVE_METADATA,
//                    4 HAVE_ENOUGH_DATA (frame decoded + tracks ready).
//                    The intermediate 2/3 states are never reported.
// v.networkState   — 0 NETWORK_EMPTY (no src), 1 NETWORK_IDLE (loaded),
//                    3 NETWORK_NO_SOURCE (src set but open failed).
//                    2 (NETWORK_LOADING) never occurs — loads are synchronous.
//
// Attribute-reflected flags:
// v.autoplay — reflects the attribute only; the engine does NOT auto-start
//              playback. Call v.play() explicitly (e.g. on 'canplaythrough').
// v.controls — reflected only; no built-in control chrome is rendered.
//              Build controls in the DOM and drive play()/pause().
// v.loop     — reflects the `loop` attribute; ASSIGN IT FROM SCRIPT
//              (v.loop = true) to actually arm pipeline looping — the setter
//              is what forwards the flag to the pipeline, so a markup-only
//              `loop` attribute does not loop by itself. While looping, the
//              stream rewinds and resumes at the end instead of firing
//              'ended' (spec behavior).
// v.preload  — reflected, default "metadata"; informational (local files
//              are opened fully at load).

// ---------------------------------------------------------------------------
// Media events
// ---------------------------------------------------------------------------
//
// Non-bubbling, trusted events on the element (addEventListener or on* via
// attributes is up to the app; there are no onplay-style IDL properties):
//
//   loadedmetadata  — after a successful load; dimensions/duration readable.
//   durationchange  — alongside loadedmetadata (and if duration changes on
//                     reload).
//   canplay         — right after loadedmetadata.
//   canplaythrough  — once the first frame is decoded (local file + audio
//                     predecode ⇒ guaranteed play-through).
//   play / pause    — state transitions from play()/pause().
//   seeking, seeked — around a currentTime assignment (same tick).
//   timeupdate      — while playing, throttled to ~250 ms of media time;
//                     also once after each seek.
//   waiting/playing — decoder stall at the edge of decoded data / recovery.
//   ended           — pipeline drained and last frame decoded (not while
//                     v.loop is set).
//   volumechange    — volume or muted actually changed.
//   ratechange      — playbackRate actually changed.
//
// Delivery: events are pumped on the main thread once per frame in windowed
// mode; in headless they flow during flush() / advanceTime() / wallSleep()
// (any script-driven drain), never in the middle of unrelated JS.

v.addEventListener('canplaythrough', () => v.play());
v.addEventListener('ended', () => console.log('done'));

// ---------------------------------------------------------------------------
// Recipe: play a clip and verify frames advance (headless)
// ---------------------------------------------------------------------------

const stage = document.getElementById('root');
stage.innerHTML = '<video id="v" src="clip.webm" style="width:320px"></video>';
flush();
const vid = document.getElementById('v');
assert(vid.readyState >= 1, 'metadata loaded');
vid.muted = true;
vid.play();
wallSleep(300);          // wall clock — video time ignores advanceTime()
flush();                 // pump media events + present the current frame
assert(vid.currentTime > 0, 'playback advanced');
vid.pause();
