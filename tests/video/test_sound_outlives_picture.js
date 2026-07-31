// A file whose sound outlives its picture plays to the end of the sound.
//
// The pipeline's end-of-stream flag means "the PICTURES have run out": the
// demuxer has no more packets and the decoder has been drained. That was taken
// for the whole of "the resource is over" for as long as every file's picture
// and sound ended together — and they do not. A soundtrack routinely runs a
// fraction of a second past the last picture, and a short animation over a
// longer piece of music is an ordinary file to be handed a <video>.
//
// What that cost: 'ended' fired at the last picture, ElVideo paused the
// pipeline and stopped the audio, and the rest of the sound was never heard.
// Measured through the ffmpeg backend before the fix, on 1 s of h264 beside 6 s
// of aac: duration read back as 1, and playback stopped at 0.96 s.
//
// The fix is in two halves and this exercises both. VideoPipeline reports the
// resource as the longest track it opened rather than the video track's own
// length; ElVideo::isEnded() requires the CLOCK to have reached that length as
// well as the pictures having run out. The clock and not currentTime, because
// the last picture's timestamp falls one presentation interval short of the
// declared duration by construction — gating on that is a file that never ends.
//
// Deliberately real seconds and not advanceTime(): the media clock is a host
// steady_clock anchor, so virtual time does not move it. The lengths here are
// the smallest that separate the two behaviours by a comfortable margin.

const os = require('os');
const path = require('path');
const fs = require('fs');

const RATE = 48000;
const FPS = 10;
const PICTURE_SECONDS = 0.4;      // 4 frames
const SOUND_SECONDS = 2.0;

const src = path.join(os.tmpdir(), 'bro_sound_outlives_' + Date.now() + '.webm')
                .split('\\').join('/');

// ── one short run of pictures, one long run of sound, in one file ─────────
{
    const enc = new VideoEncoder({
        path: src, width: 64, height: 64, fps: FPS, fpsDen: 1,
        quality: 'realtime',
        audioSampleRate: RATE, audioChannels: 1, audioBitrateKbps: 64,
    });
    const px = new Uint8Array(64 * 64 * 4);
    for (let f = 0; f < FPS * PICTURE_SECONDS; ++f) {
        for (let i = 0; i < 64 * 64; ++i) {
            px[i * 4] = (f * 60) & 255; px[i * 4 + 1] = 90; px[i * 4 + 2] = 180;
            px[i * 4 + 3] = 255;
        }
        enc.addFrameRGBA(px);
    }
    const n = Math.round(RATE * SOUND_SECONDS);
    const pcm = new Float32Array(n);
    for (let i = 0; i < n; ++i) pcm[i] = 0.25 * Math.sin(2 * Math.PI * 440 * i / RATE);
    enc.addAudioFramesPCM(pcm, n);
    enc.finish();
    assert(fs.existsSync(src), 'encoded a clip whose sound outlives its picture');
}

const v = document.createElement('video');
document.body.appendChild(v);
let endedAt = -1;
v.addEventListener('ended', () => { endedAt = v.currentTime; });

let ready = false, failed = false;
v.addEventListener('loadedmetadata', () => { ready = true; });
v.addEventListener('error', () => { failed = true; });
v.src = src;

function pump(ms) {
    const until = Date.now() + ms;
    while (Date.now() < until) { sleep(10); flush(); advanceTime(10); }
}

{
    const t = Date.now();
    while (!ready && !failed && Date.now() - t < 15000) pump(20);
    assert(ready, 'the file opens');
}

// ── the length is the resource's, not the video track's ───────────────────
assert(Math.abs(v.duration - SOUND_SECONDS) < 0.1,
       'duration is the whole resource (' + SOUND_SECONDS + 's), got ' +
       v.duration.toFixed(3) + 's');
assert(!v.ended, 'and a freshly loaded file has not ended');

// ── past the last picture, and still playing ──────────────────────────────
// This is the assertion the old rule failed: at 1 s the pictures ran out half a
// second ago, and the element used to have fired 'ended' and paused itself.
v.play();
pump(1000);
assert(!v.ended, "past the last picture the element has not ended, at " +
                 v.currentTime.toFixed(3) + 's');
assert(!v.paused, 'and it is still playing');
assert(endedAt < 0, "and 'ended' has not fired");

// The picture that is on screen is the last one there is, and it stays: a
// frozen final frame under continuing sound is the correct presentation, not a
// black box.
assert(v.videoWidth === 64, 'the last picture is still the one being shown');

// The position keeps moving with the sound. Frozen on the last picture's
// timestamp, an element playing on is indistinguishable from a stalled one:
// nothing reaches duration, no more 'timeupdate' fires, and 'ended' arrives out
// of nowhere. Past the pictures the clock is what says where playback is.
assert(v.currentTime > PICTURE_SECONDS + 0.2,
       'and the position has moved past the last picture, to ' +
       v.currentTime.toFixed(3) + 's');

// ── the end of the sound is the end of the file ───────────────────────────
pump(1600);
assert(v.ended, 'the file ends when its sound does, at ' + v.currentTime.toFixed(3) + 's');
assert(endedAt >= 0, "and 'ended' fired");

// ── seeking back re-arms it, as it does for any other file ────────────────
v.currentTime = 0;
assert(!v.ended, 'seeking back off the end un-ends it');

v.remove();
try { fs.unlinkSync(src.split('/').join(path.sep)); } catch (e) {}

console.log('PASS: a file whose sound outlives its picture plays to the end of the sound');
