// video.stepFrame() — moving by pictures instead of by time.
//
// The bug this guards: a player has no way to step a frame except
// `currentTime += 1/fps`, and that does not work. A nominal frame rate is an
// average, and the seconds round trip through a double misses the frame
// boundary by nanoseconds, so a backward step lands on the frame it started
// from and nothing moves. Stepping has to come from the file's own timestamps.
//
// Also covers the seek contract stepping depends on: seeking to time T shows
// the frame T falls INSIDE — the last one at or before it — not the next one
// up.

const os = require('os');
const path = require('path');
const fs = require('fs');

// ── a file with known frame times ─────────────────────────────────────────
const W = 64, H = 64, FPS = 10, N = 24;
const src = path.join(os.tmpdir(), 'bro_step_' + Date.now() + '.webm');

const enc = new VideoEncoder({ path: src, width: W, height: H, fps: FPS,
                               quality: 'realtime' });
const px = new Uint8Array(W * H * 4);
for (let f = 0; f < N; ++f) {
    px.fill(255);
    for (let i = 0; i < W * H; ++i) px[i * 4] = (f * 10) & 0xff;   // red ramp
    enc.addFrameRGBA(px);
}
enc.finish();
assert(fs.existsSync(src), 'encoded a test clip');

const v = document.createElement('video');
document.body.appendChild(v);
// The decoder is attached to the element on the engine's next dirty pass, so
// a freshly created <video> has nothing behind it until the tree settles.
flush();
v.src = src;

assert(v.readyState >= 1, 'metadata ready after src assignment');
assert(v.videoWidth === W, `videoWidth ${v.videoWidth} = ${W}`);

const FRAME = 1 / FPS;
const near = (a, b, tol) => Math.abs(a - b) <= (tol === undefined ? 0.002 : tol);

// ── a seek shows the frame the instant falls inside ───────────────────────
v.currentTime = 5 * FRAME + FRAME / 2;          // halfway through frame 5
assert(near(v.currentTime, 5 * FRAME),
       `seek mid-frame shows frame 5 (${v.currentTime.toFixed(3)}s)`);

v.currentTime = 7 * FRAME;                       // exactly on frame 7
assert(near(v.currentTime, 7 * FRAME),
       `seek on a boundary shows that frame (${v.currentTime.toFixed(3)}s)`);

// ── one step forward, one step back, and we are where we started ──────────
const t0 = v.currentTime;

assert(v.stepFrame(1) === 1, 'stepFrame(1) reports one frame moved');
assert(near(v.currentTime, t0 + FRAME),
       `forward step lands on the next frame (${v.currentTime.toFixed(3)}s)`);

assert(v.stepFrame(-1) === 1, 'stepFrame(-1) reports one frame moved');
assert(near(v.currentTime, t0),
       `back step returns to where it started (${v.currentTime.toFixed(3)}s)`);

// Repeating it must keep moving — the failure mode was a step that silently
// did nothing every time.
let t = v.currentTime;
for (let i = 0; i < 5; ++i) {
    v.stepFrame(-1);
    assert(v.currentTime < t - FRAME / 2,
           `back step ${i + 1} moved (${t.toFixed(3)} -> ${v.currentTime.toFixed(3)})`);
    t = v.currentTime;
}

// ── a long walk out and back is exactly reversible ────────────────────────
const before = v.currentTime;
assert(v.stepFrame(8) === 8, 'stepFrame(8) moved eight');
assert(near(v.currentTime, before + 8 * FRAME, 0.005),
       `eight forward = eight frames (${v.currentTime.toFixed(3)}s)`);
assert(v.stepFrame(-8) === 8, 'stepFrame(-8) moved eight');
assert(near(v.currentTime, before),
       `eight back returns exactly (${v.currentTime.toFixed(3)}s)`);

// ── back to the very start, one frame at a time ───────────────────────────
// Backward stepping used to stall at a keyframe: the step asked for a target
// one nanosecond earlier, which rounds to nothing in the container's timebase,
// so the seek landed back on the frame it was trying to leave and the walk
// stopped dead there.
v.currentTime = v.duration * 0.8;
let prev = v.currentTime;
let walked = 0;
while (v.stepFrame(-1) === 1) {
    assert(v.currentTime < prev,
           `walk back stalled at ${prev.toFixed(3)}s after ${walked} frames`);
    prev = v.currentTime;
    walked++;
    if (walked > N * 2) break;
}
assert(near(prev, 0), `walked ${walked} frames back to the start (${prev.toFixed(3)}s)`);
assert(walked >= N / 2, `and that was most of the file (${walked} frames)`);

// ── the ends of the file report honestly ──────────────────────────────────
v.currentTime = 0;
assert(near(v.currentTime, 0), 'seek to 0 shows the first frame');
assert(v.stepFrame(-1) === 0, 'no frame before the first');
assert(near(v.currentTime, 0), 'a refused step leaves the picture alone');

v.currentTime = v.duration;
const atEnd = v.currentTime;
assert(atEnd > 0, `seek to duration lands on a real frame (${atEnd.toFixed(3)}s)`);
assert(v.stepFrame(1) === 0, 'no frame after the last');
assert(near(v.currentTime, atEnd), 'a refused step leaves the picture alone');

// ── frameRate is reported for display, when the container declares one ────
assert(typeof v.frameRate === 'number', 'frameRate is a number');
assert(v.frameRate === 0 || near(v.frameRate, FPS, 1),
       `frameRate is 0 or about ${FPS} (got ${v.frameRate})`);

try { fs.unlinkSync(src); } catch (e) {}
console.log('PASS');
