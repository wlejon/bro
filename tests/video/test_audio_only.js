// A file with no picture in it plays.
//
// The pipeline drove its clock from decoded pictures, so a source with no
// video track was refused at adoption and an audio-only file could not be
// opened at all: readyState stayed 0, networkState reported NO_SOURCE, and
// nothing played. The sound was never the problem — bro.media.peaks() has
// always read the same files happily — it was that "how far along are we"
// had only one answer and it was a frame timestamp.
//
// With no video track the position comes from the media clock the sound is
// anchored to instead. That is a FALLBACK and is taken only when there is no
// picture: where there is one the pictures decide, always, because two clocks
// arbitrating is how A/V sync bugs are born.
//
// The element simply has no picture: videoWidth/videoHeight are 0, while the
// box keeps the 300x150 replaced-element fallback so a playing element does
// not collapse out of the page.

const os = require('os');
const path = require('path');
const fs = require('fs');

const RATE = 48000;
const SECONDS = 0.8;

// ── a WebM with an Opus track and no video track at all ───────────────────
// Omitting width and height with a sample rate set is what asks for one.

const src = path.join(os.tmpdir(), 'bro_audio_only_' + Date.now() + '.webm');
const enc = new VideoEncoder({ path: src, audioSampleRate: RATE,
                               audioChannels: 1, audioBitrateKbps: 64 });
const N = Math.round(RATE * SECONDS);
const pcm = new Float32Array(N);
for (let i = 0; i < N; ++i) pcm[i] = 0.3 * Math.sin(2 * Math.PI * 440 * i / RATE);
enc.addAudioFramesPCM(pcm, N);
enc.finish();
assert(fs.existsSync(src), 'encoded a sound-only clip');
assert(fs.statSync(src).size > 1000, 'and it has something in it');

// Frames are refused rather than quietly dropped: there is no track for them.
let threw = false;
try {
    const px = new Uint8Array(4);
    const e2 = new VideoEncoder({ path: src + '.2', audioSampleRate: RATE,
                                  audioChannels: 1 });
    threw = (e2.addFrameRGBA(px) === false);
    e2.finish();
    try { fs.unlinkSync(src + '.2'); } catch (e) {}
} catch (e) { threw = true; }
assert(threw, 'a sound-only encoder refuses picture frames');

// ── it opens, and it says it has no picture ───────────────────────────────

const v = document.createElement('video');
document.body.appendChild(v);
flush();
v.src = src;

assert(v.networkState === 1, 'the file loaded (networkState ' + v.networkState + ')');
assert(v.readyState === 4,
       'and is ready to play — there is no frame to wait for (readyState ' +
       v.readyState + ')');
assert(v.videoWidth === 0 && v.videoHeight === 0,
       'no picture: videoWidth/videoHeight are 0 (got ' +
       v.videoWidth + 'x' + v.videoHeight + ')');
assert(v.videoRotation === 0, 'and nothing is rotated');
assert(Math.abs(v.duration - SECONDS) < 0.15,
       'duration comes from the audio track (' + v.duration.toFixed(3) +
       's, want about ' + SECONDS + ')');
assert(v.currentTime === 0, 'and it starts at the beginning');

// The box is still a box. A replaced element with no intrinsic size gets the
// spec's 300x150, which is what keeps it laid out at all.
flush();
const box = v.getBoundingClientRect();
assert(box.width > 0 && box.height > 0,
       'the element still has a box (' + box.width + 'x' + box.height + ')');

// ── seeking ───────────────────────────────────────────────────────────────

v.currentTime = 0.4;
assert(Math.abs(v.currentTime - 0.4) < 0.01,
       'seek lands where it was asked (' + v.currentTime.toFixed(3) + 's)');
v.currentTime = 0;
assert(v.currentTime < 0.01, 'and back to the start');

// There are no frames, so there is no stepping between them.
assert(v.stepFrame(1) === 0, 'stepFrame does nothing with no pictures to step');

// ── currentTime advances while it plays ───────────────────────────────────
// Video runs on the host wall clock, so waiting means wallSleep, not
// advanceTime. flush() is what pumps the pipeline in headless.

function pump(ms) { wallSleep(ms); flush(); }

let playFired = 0;
v.addEventListener('play', function () { playFired++; });
let timeUpdates = 0;
v.addEventListener('timeupdate', function () { timeUpdates++; });
let endedFired = 0;
v.addEventListener('ended', function () { endedFired++; });

v.play();
assert(!v.paused, 'it reports playing');
pump(250);
const t1 = v.currentTime;
assert(t1 > 0.05, 'currentTime moved while playing (' + t1.toFixed(3) + 's)');
assert(t1 < v.duration, 'and has not run past the end yet');

// ── pause freezes it ──────────────────────────────────────────────────────

v.pause();
assert(v.paused, 'it reports paused');
const held = v.currentTime;
pump(150);
assert(Math.abs(v.currentTime - held) < 0.02,
       'a paused clip does not move (' + held.toFixed(3) + ' -> ' +
       v.currentTime.toFixed(3) + ')');

// ── it ends, once, and says so ────────────────────────────────────────────

v.play();
for (let i = 0; i < 40 && endedFired === 0; ++i) pump(60);
assert(endedFired === 1, 'ended fired exactly once (got ' + endedFired + ')');
assert(v.ended, 'and the element says it ended');
assert(v.paused, 'which pauses it');
assert(Math.abs(v.currentTime - v.duration) < 0.05,
       'the playhead finished at the end (' + v.currentTime.toFixed(3) + 's of ' +
       v.duration.toFixed(3) + ')');
assert(timeUpdates > 0, 'timeupdate fired along the way (' + timeUpdates + ')');
assert(playFired >= 1, 'play fired');

// A seek away from the end re-arms it.
v.currentTime = 0;
assert(!v.ended, 'seeking back off the end clears ended');

// ── a file with a picture is completely unaffected ────────────────────────
// The audio clock is a fallback and nothing else; the same assertions the
// rest of the video suite makes still hold next to it.

const W = 64, H = 32, FPS = 10;
const withPicture = path.join(os.tmpdir(), 'bro_audio_only_v_' + Date.now() + '.webm');
const enc2 = new VideoEncoder({ path: withPicture, width: W, height: H, fps: FPS,
                                quality: 'realtime' });
const px = new Uint8Array(W * H * 4);
px.fill(200);
for (let f = 0; f < 12; ++f) enc2.addFrameRGBA(px);
enc2.finish();

const v2 = document.createElement('video');
document.body.appendChild(v2);
flush();
v2.src = withPicture;
assert(v2.videoWidth === W && v2.videoHeight === H,
       'a file with a picture still reports its size');
assert(v2.readyState === 4, 'and is ready');
v2.currentTime = 5 / FPS;
assert(Math.abs(v2.currentTime - 5 / FPS) < 0.002,
       'and still reads its position back snapped to a frame boundary (' +
       v2.currentTime.toFixed(3) + 's)');
assert(v2.stepFrame(1) === 1, 'and still steps by pictures');

v.remove();
v2.remove();
try { fs.unlinkSync(src); } catch (e) {}
try { fs.unlinkSync(withPicture); } catch (e) {}
console.log('PASS: audio-only <video>');
