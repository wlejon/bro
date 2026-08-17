// The video probe: VideoEncoder and GifEncoder from compiled code.
//
// Recording is the one capability in this layer an app cannot build for
// itself — it cannot implement VP9, and it cannot read the composited
// framebuffer — so what matters is less "does a file appear" (the runner
// checks that) than that every way of handing pixels over behaves, and that
// every way of handing them over WRONG is refused with the right kind of
// error. Five claims:
//
//   1. The typed-array path encodes, and framesWritten after finish() is the
//      number of frames pushed. Before finish() it is not, because libvpx
//      buffers — which is exactly why it is read after.
//
//   2. The viewport path encodes. For a compiled app this is not one capture
//      route among several, it is THE one: the host's own canvas has no 2D
//      context, so addCanvasFrame can only ever refuse a canvas the app made.
//
//   3. The 2D-canvas path encodes when the canvas came from the PAGE. This is
//      the mixed app — interpreted UI, compiled logic — and it is the only way
//      a 2D surface reaches this side at all.
//
//   4. Every refusal is the RIGHT ERROR. TypeError for the wrong kind of
//      thing, RangeError for the wrong size, Error for a state problem. The
//      kind is API: an app retries a RangeError by resizing and cannot do
//      anything useful with a TypeError, and a layer that answered plain Error
//      to both would take that choice away.
//
//   5. An audio-only encoder refuses frames rather than dropping them. A file
//      with a soundtrack and no picture, silently, is the worst outcome here.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// The name of whatever a call threw, or 'none' if it did not throw. The NAME
// and not the message: the message is prose this layer is free to improve, the
// name is the contract an app branches on.
function threw(fn) {
    try { fn(); } catch (e) { return e && e.name ? e.name : 'unknown'; }
    return 'none';
}

const W = 64, H = 48;

// ---------------------------------------------------------------------------
// The globals exist
// ---------------------------------------------------------------------------

say('typeof.VideoEncoder', typeof VideoEncoder);
say('typeof.GifEncoder', typeof GifEncoder);

// ---------------------------------------------------------------------------
// Claim 4, the constructor half
// ---------------------------------------------------------------------------

say('refuse.noConfig', threw(function () { new VideoEncoder('out.webm'); }));
say('refuse.noPath', threw(function () { new VideoEncoder({ width: W, height: H }); }));
// 4:2:0 chroma needs even dimensions, so libvpx refuses the config and the
// file is never opened. A state problem rather than a wrong argument kind.
say('refuse.oddWidth', threw(function () {
    new VideoEncoder({ path: 'bronze_video_odd.webm', width: 65, height: H });
}));

// ---------------------------------------------------------------------------
// Claim 1: the typed-array path
// ---------------------------------------------------------------------------

const enc = new VideoEncoder({
    path: 'bronze_video_probe.webm',
    width: W, height: H, fps: 10, quality: 'realtime',
});
say('enc.width', enc.width);
say('enc.height', enc.height);
say('enc.frames0', enc.framesWritten);

const frame = new Uint8Array(W * H * 4);
for (let f = 0; f < 3; f++) {
    for (let i = 0; i < W * H; i++) {
        const p = i * 4;
        frame[p] = (i + f * 40) & 0xff;
        frame[p + 1] = (f * 60) & 0xff;
        frame[p + 2] = 128;
        frame[p + 3] = 255;
    }
    enc.addFrameRGBA(frame);
}

// Claim 4, the frame half.
say('refuse.notArray', threw(function () { enc.addFrameRGBA([1, 2, 3, 4]); }));
say('refuse.shortBuffer', threw(function () { enc.addFrameRGBA(new Uint8Array(16)); }));
say('refuse.noAudioTrack', threw(function () {
    enc.addAudioFramesPCM(new Float32Array(480));
}));

// ---------------------------------------------------------------------------
// Claim 5: an encoder with sound and no picture
// ---------------------------------------------------------------------------

const aenc = new VideoEncoder({
    path: 'bronze_video_audio.webm',
    audioSampleRate: 24000, audioChannels: 1, audioBitrateKbps: 96,
});
const pcm = new Float32Array(4800);   // 200 ms, a whole number of 20 ms packets
for (let i = 0; i < pcm.length; i++) {
    pcm[i] = 0.2 * Math.sin(2 * Math.PI * 440 * i / 24000);
}
say('audio.push', aenc.addAudioFramesPCM(pcm));
// A Uint8Array of plausible length would otherwise be read as garbage floats
// and encoded as noise — a bug you can hear and cannot see.
say('refuse.notFloat', threw(function () { aenc.addAudioFramesPCM(new Uint8Array(960)); }));
say('refuse.noVideoTrack', threw(function () { aenc.addFrameRGBA(frame); }));
say('audio.finish', aenc.finish());

// ---------------------------------------------------------------------------
// Claim 4, the canvas half
// ---------------------------------------------------------------------------

say('refuse.notElement', threw(function () { enc.addCanvasFrame({}); }));
say('refuse.notCanvas', threw(function () {
    enc.addCanvasFrame(document.getElementById('notacanvas'));
}));

// A canvas with a WebGL context. The context is created HERE rather than in
// the page so the refusal is about a canvas this layer itself handed out.
const glCanvas = document.getElementById('gl');
const gl = glCanvas.getContext('webgl2');
say('gl.gotContext', gl !== null);
say('refuse.webglCanvas', threw(function () { enc.addCanvasFrame(glCanvas); }));

// ---------------------------------------------------------------------------
// Claim 3: the page's 2D canvas
// ---------------------------------------------------------------------------

document.dispatchEvent({ type: 'app:paint' });
const paint = document.getElementById('paint');
say('paint.tag', paint.tagName);

const cenc = new VideoEncoder({
    path: 'bronze_video_canvas.webm', width: W, height: H, fps: 10,
});
say('canvas.encode', cenc.addCanvasFrame(paint));
say('canvas.finish', cenc.finish());
say('canvas.frames', cenc.framesWritten);

// The size check is a RangeError, and it is the one an app can act on.
const wrong = new VideoEncoder({
    path: 'bronze_video_wrong.webm', width: W + 2, height: H, fps: 10,
});
say('refuse.canvasSize', threw(function () { wrong.addCanvasFrame(paint); }));
wrong.finish();

// ---------------------------------------------------------------------------
// Claim 2: the viewport
// ---------------------------------------------------------------------------

// Pinned, and NOT from the fixture: bro-headless has its own --width/--height
// with a 1920x1080 default and assigns them over whatever bro.json said
// (engine/headless_driver.cpp), so the appdir's 640x480 is dead here and every
// headless run is this size on every machine. Printed rather than assumed so
// that a runner which someday passes --width shows up as itself instead of as
// a mysterious RangeError two lines later.
say('vp.size', window.innerWidth + 'x' + window.innerHeight);

const gif = new GifEncoder({
    path: 'bronze_video_probe.gif',
    width: window.innerWidth, height: window.innerHeight, fps: 10,
});
for (let f = 0; f < 3; f++) gif.addViewportFrame();
// A GIF writes each frame as it arrives, so unlike the webm encoder this
// number means something before finish().
say('gif.frames', gif.framesWritten);
say('gif.delay', threw(function () { gif.setNextFrameDelayCs(50); }));
say('gif.finish', gif.finish());

const vpBad = new GifEncoder({
    path: 'bronze_video_vpbad.gif',
    width: window.innerWidth + 8, height: window.innerHeight, fps: 10,
});
say('refuse.viewportSize', threw(function () { vpBad.addViewportFrame(); }));
vpBad.finish();

// ---------------------------------------------------------------------------
// Closing the webm encoder, and what framesWritten means afterwards
// ---------------------------------------------------------------------------

say('enc.finish', enc.finish());
// Only now: before the flush this counts muxed packets and libvpx is still
// holding some. After it, every frame pushed has been written.
say('enc.frames1', enc.framesWritten);
say('enc.err', enc.lastError === '' ? 'none' : enc.lastError);
// Idempotent, so a cleanup path that runs twice is not an error.
say('enc.finishAgain', enc.finish());

// Printed last, so a probe that died halfway is a missing line rather than a
// silently short but otherwise matching output.
say('done', 1);
