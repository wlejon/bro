// Disk-stream position and length: a stream knows how long its file is
// (getStreamDuration / getStreamStats().duration), getPlaybackPosition reads
// 0..1 of it, getPlaybackPositionSeconds stays inside the file while a
// looping stream wraps, and isClipPlaying turns false once a non-looping
// stream has played out.
//
// Pacing as in test_stream_file.js: the decode worker is a real thread while
// sleep() renders virtual time, so every virtual gulp waits until the ring
// holds that much (or the file is fully decoded).

const os = require('os');
const path = require('path');

const ctx = new AudioContext();
const sr = ctx.sampleRate;
const wavPath = path.join(os.tmpdir(), 'bro_test_stream_position.wav');

function realWait(ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {}
}

function waitForStats(id, pred, timeoutMs) {
    const t0 = Date.now();
    while (Date.now() - t0 < (timeoutMs || 5000)) {
        const st = ctx.getStreamStats(id);
        if (st && pred(st)) return st;
        realWait(5);
    }
    return null;
}

// Render `ms` of virtual time in 20 ms gulps, each fed by the worker first.
function playFed(id, ms, looping) {
    const gulp = Math.ceil(sr * 0.02);
    for (let t = 0; t < ms; t += 20) {
        waitForStats(id, s => s.bufferedFrames >= gulp + 1024 || (!looping && s.finished) ||
                              (!looping && s.decodedFrames >= fileFrames), 5000);
        sleep(20);
    }
}

// 0.4 s, 440 Hz at the engine rate.
const fileSec = 0.4;
const fileFrames = Math.round(sr * fileSec);
const tone = new Float32Array(fileFrames);
for (let i = 0; i < tone.length; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
assert(ctx.saveWav(wavPath, tone, 1, sr), 'wrote the position fixture WAV');

// --- duration ---------------------------------------------------------------
{
    const id = ctx.createStreamFromFile(wavPath);
    const dur = ctx.getStreamDuration(id);
    assert(Math.abs(dur - fileSec) < 1e-3, 'getStreamDuration is the file length, got ' + dur);
    const st = ctx.getStreamStats(id);
    assert(st && Math.abs(st.duration - fileSec) < 1e-3, 'getStreamStats().duration, got ' + JSON.stringify(st));
    assert(ctx.getStreamDuration(-5) === 0, 'getStreamDuration of an unknown id is 0');
    const live = ctx.createStream(1);
    assert(ctx.getStreamDuration(live) === 0, 'a live stream has no duration');
    ctx.closeStream(live);
    ctx.closeStream(id);
}

// MP3 without a Xing header: the length comes from a frame-header scan.
{
    const mp3 = path.resolve('tests/audio/fixtures/quiet_loud.mp3');
    const id = ctx.createStreamFromFile(mp3);
    const dur = ctx.getStreamDuration(id);
    assert(dur > 1.95 && dur < 2.1, 'MP3 stream duration ~2 s, got ' + dur);
    ctx.closeStream(id);
}

// --- looping: position wraps inside the file --------------------------------
{
    const id = ctx.createStreamFromFile(wavPath, { loop: true });
    assert(waitForStats(id, s => s.bufferedFrames >= sr / 4), 'looping stream prebuffered');

    let maxSec = 0, maxNorm = 0, minNorm = 1, samples = 0;
    const gulp = Math.ceil(sr * 0.02);
    for (let t = 0; t < 1820; t += 20) {
        waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
        sleep(20);
        const s = ctx.getPlaybackPositionSeconds(id);
        const n = ctx.getPlaybackPosition(id);
        maxSec = Math.max(maxSec, s);
        maxNorm = Math.max(maxNorm, n);
        minNorm = Math.min(minNorm, n);
        samples++;
        assert(Math.abs(n * fileSec - s) < 1e-3, 'normalized position is seconds / duration (' + n + ' vs ' + s + ')');
    }
    const st = ctx.getStreamStats(id);
    const played = st.playedFrames / sr;
    console.log('looped ' + played.toFixed(3) + ' s, max position ' + maxSec.toFixed(3) + ' s');
    assert(played > 1.6, 'played more than four passes, ' + played);
    assert(maxSec < fileSec + 1e-6, 'position stays inside the 0.4 s file, max ' + maxSec);
    assert(maxNorm <= 1 && minNorm >= 0, 'normalized position in 0..1');
    assert(minNorm < 0.25 && maxNorm > 0.75, 'normalized position sweeps the file, ' + minNorm + '..' + maxNorm);
    const pos = ctx.getPlaybackPositionSeconds(id);
    const expect = played % fileSec;
    assert(Math.abs(pos - expect) < 0.01, 'position is played time modulo the file, ' + pos + ' vs ' + expect);
    assert(Math.abs(st.position - pos) < 1e-9, 'getStreamStats().position matches, ' + st.position);
    assert(ctx.isClipPlaying(id), 'a looping stream keeps playing');
    ctx.closeStream(id);
}

// --- non-looping: plays, then finishes ---------------------------------------
{
    const id = ctx.createStreamFromFile(wavPath);
    assert(waitForStats(id, s => s.decodedFrames >= fileFrames - 64), 'short file fully decoded');
    assert(ctx.isClipPlaying(id), 'stream is playing once prebuffered');

    playFed(id, 200, false);
    const half = ctx.getPlaybackPosition(id);
    assert(half > 0.35 && half < 0.65, 'halfway through reads ~0.5, got ' + half);
    assert(ctx.isClipPlaying(id), 'still playing halfway');

    playFed(id, 400, false);
    const fin = waitForStats(id, s => s.finished, 5000);
    assert(fin && fin.finished, 'stream finished');
    assert(!ctx.isClipPlaying(id), 'isClipPlaying is false once a non-looping stream finished');
    const endSec = ctx.getPlaybackPositionSeconds(id);
    assert(Math.abs(endSec - fileSec) < 0.01, 'finished position reads the duration, got ' + endSec);
    assert(Math.abs(ctx.getPlaybackPosition(id) - 1) < 0.025, 'finished normalized position ~1, got ' + ctx.getPlaybackPosition(id));

    // Seeking a finished stream restarts it: playing again.
    ctx.seekPlayback(id, 0.1);
    const back = waitForStats(id, s => !s.finished && s.bufferedFrames > 0, 5000);
    assert(back, 'seek restarted the finished stream');
    sleep(20);
    assert(ctx.isClipPlaying(id), 'playing again after the seek');
    ctx.closeStream(id);
}

console.log('stream position tests passed');
