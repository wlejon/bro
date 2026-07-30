// An encoded file's declared duration includes its last frame.
//
// libwebm derives the segment Duration from the last block's TIMESTAMP, which
// is where the final frame starts, not where it ends. So every clip bro wrote
// came out exactly one frame short: 5 frames at 5 fps declared 0.8s instead of
// 1.0s, and a player looping the file dropped the last frame — the one that
// matters most in a parameter sweep, where the last frame is the endpoint being
// demonstrated. webm_encoder.cpp tracks the end of the latest packet on any
// track and hands that to Segment::set_duration, which overrides the
// computation.

const os = require('os');
const path = require('path');
const fs = require('fs');

const tmpDir = os.tmpdir();
const stamp = Date.now();

function encode(file, opts) {
  const enc = new VideoEncoder(Object.assign({ path: file, quality: 'realtime' }, opts));
  const px = new Uint8Array(opts.width * opts.height * 4);
  for (let f = 0; f < opts.frames; f++) {
    for (let i = 0; i < opts.width * opts.height; i++) {
      px[i * 4] = (f * 37) & 255; px[i * 4 + 1] = 128; px[i * 4 + 2] = 200; px[i * 4 + 3] = 255;
    }
    enc.addFrameRGBA(px);
  }
  enc.finish();
  assert(fs.existsSync(file), 'wrote ' + file);
  return file;
}

// Read the duration back through the engine's own demuxer.
function durationOf(file) {
  const v = document.createElement('video');
  v.src = file.split('\\').join('/');
  document.body.appendChild(v);
  let ready = false, failed = false;
  v.addEventListener('loadedmetadata', () => { ready = true; });
  v.addEventListener('error', () => { failed = true; });
  const t = Date.now();
  while (!ready && !failed && Date.now() - t < 15000) { sleep(20); flush(); advanceTime(20); }
  assert(ready, 'metadata loaded for ' + file);
  const d = v.duration;
  v.remove();
  return d;
}

// ── the canonical case: a sweep clip at 5 fps ─────────────────────────────
// 0.2s per frame is what a parameter walk uses, and 5/1 is exact, so the
// expected duration is unambiguous.
const five = encode(path.join(tmpDir, 'bro_dur_5_' + stamp + '.webm'),
                    { width: 64, height: 64, fps: 5, fpsDen: 1, frames: 5 });
let d = durationOf(five);
assert(Math.abs(d - 1.0) < 0.02,
       '5 frames at 5 fps declares 1.00s, got ' + d.toFixed(3) + 's');

// ── a single frame still has a duration ───────────────────────────────────
// The degenerate case the old computation got worst: one frame's timestamp is
// 0, so the file declared a duration of zero seconds.
const one = encode(path.join(tmpDir, 'bro_dur_1_' + stamp + '.webm'),
                   { width: 64, height: 64, fps: 5, fpsDen: 1, frames: 1 });
d = durationOf(one);
assert(Math.abs(d - 0.2) < 0.02,
       'a 1-frame 5 fps clip declares 0.20s, got ' + d.toFixed(3) + 's');

// ── a rational frame rate ─────────────────────────────────────────────────
// fps=20 fpsDen=3 is 6.667 fps (150 ms per frame) — 9 frames = 1.35s.
const rat = encode(path.join(tmpDir, 'bro_dur_r_' + stamp + '.webm'),
                   { width: 64, height: 64, fps: 20, fpsDen: 3, frames: 9 });
d = durationOf(rat);
assert(Math.abs(d - 1.35) < 0.03,
       '9 frames at 20/3 fps declares 1.35s, got ' + d.toFixed(3) + 's');

// ── 30 fps, the default, over a longer clip ───────────────────────────────
const many = encode(path.join(tmpDir, 'bro_dur_m_' + stamp + '.webm'),
                    { width: 64, height: 64, fps: 30, fpsDen: 1, frames: 30 });
d = durationOf(many);
assert(Math.abs(d - 1.0) < 0.02,
       '30 frames at 30 fps declares 1.00s, got ' + d.toFixed(3) + 's');

[five, one, rat, many].forEach((f) => { try { fs.unlinkSync(f); } catch (e) {} });
console.log('PASS: declared duration covers the final frame');
