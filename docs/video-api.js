// =============================================================================
// bro Video / GIF Encoder API
// =============================================================================
//
// VideoEncoder writes a single-track WebM file containing a VP9 video stream.
// Frames are RGBA in, encoded with libvpx (software). Audio tracks aren't
// supported yet — add when the calling-app work needs them.
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
