// Playback seek (ctx.seekPlayback) + seconds-domain position
// (ctx.getPlaybackPositionSeconds) for clip playbacks and disk streams,
// plus the ctx.outputLatency estimate.

const fs = require('fs');
const os = require('os');
const path = require('path');

const ctx = new AudioContext();
const sr = ctx.sampleRate;

// --- outputLatency: number, 0 in headless (no device) -----------------------
assert(typeof ctx.outputLatency === 'number', 'outputLatency is a number');
assert(ctx.outputLatency >= 0, 'outputLatency is non-negative');
console.log('outputLatency:', ctx.outputLatency);

// --- clip playback: seconds position tracks rendering ------------------------
const tone = new Float32Array(sr * 2);
for (let i = 0; i < tone.length; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
const clip = ctx.createClip(tone, 1);
assert(clip >= 0, 'clip created');

{
    const id = ctx.playClip(clip, 1.0, false);
    sleep(100);  // renders ~100 ms of virtual audio
    const p = ctx.getPlaybackPositionSeconds(id);
    assert(Math.abs(p - 0.1) < 0.02, 'position ~0.1s after 100ms render, got ' + p);

    // Seek forward: cursor lands immediately, rendering continues from there.
    ctx.seekPlayback(id, 1.0);
    const q = ctx.getPlaybackPositionSeconds(id);
    assert(Math.abs(q - 1.0) < 0.02, 'position ~1.0s right after seek, got ' + q);
    sleep(100);
    const r = ctx.getPlaybackPositionSeconds(id);
    assert(Math.abs(r - 1.1) < 0.03, 'position ~1.1s after seek + 100ms, got ' + r);
    ctx.stopPlayback(id);
}

// --- clip playback: seek clamps to the region --------------------------------
{
    const id = ctx.playClip(clip, 1.0, true);
    ctx.seekPlayback(id, 100);   // far past the 2 s clip
    const p = ctx.getPlaybackPositionSeconds(id);
    assert(p <= 2.0 && p > 1.9, 'seek past end clamps inside clip, got ' + p);
    ctx.seekPlayback(id, -3);
    const q = ctx.getPlaybackPositionSeconds(id);
    assert(Math.abs(q) < 0.02, 'negative seek clamps to 0, got ' + q);
    ctx.stopPlayback(id);
}

// --- disk stream: seek is decoder-side file time ------------------------------
// File layout: first 2 s silence, then 2 s of loud tone. Seeking to 3 s must
// produce audio promptly — playing from 0 would stay silent for 2 s.
const wavPath = path.join(os.tmpdir(), 'bro_test_seek_tone.wav');
{
    const pcm = new Float32Array(sr * 4);
    for (let i = sr * 2; i < pcm.length; i++)
        pcm[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
    assert(ctx.saveWav(wavPath, pcm, 1, sr), 'wrote seek fixture WAV');

    function realWait(ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) {} }
    function waitFor(pred, timeoutMs) {
        const t0 = Date.now();
        while (Date.now() - t0 < (timeoutMs || 5000)) {
            if (pred()) return true;
            realWait(10);
        }
        return false;
    }

    const id = ctx.createStreamFromFile(wavPath);
    assert(id >= 0, 'stream opened');
    assert(waitFor(() => ctx.getStreamStats(id).bufferedFrames > 0), 'prebuffered');

    ctx.seekPlayback(id, 3.0);

    // Render small virtual blocks until post-seek tone shows on the master
    // meter (the worker needs real wall time to seek + refill).
    const heard = waitFor(() => {
        sleep(20);
        return ctx.getBusPeakL(0) > 0.05 || ctx.getBusPeakR(0) > 0.05;
    });
    assert(heard, 'tone heard shortly after seeking into the tone region');

    const pos = ctx.getPlaybackPositionSeconds(id);
    assert(pos >= 2.9 && pos <= 4.2, 'stream position reports file time after seek, got ' + pos);

    ctx.closeStream(id);
    fs.unlinkSync(wavPath);
}

console.log('test_playback_seek: all assertions passed');
