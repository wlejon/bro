// Ogg Opus: decodeAudioData, createClipFromFile and createStreamFromFile on
// real Opus files (fixtures/, made by ffmpeg/libopus):
//   tone_silence_stereo.opus  3 s, 48 kHz stereo, L=440 Hz R=880 Hz at 0.5
//                             for 1.5 s then silence; 100 ms Ogg pages, so a
//                             seek bisects across ~30 pages
//   quiet_loud.opus           2 s, 48 kHz mono, silent 1 s then 440 Hz at 0.6
// Length comes from the granule positions, so it is exact: 3 s is 144000
// frames at 48 kHz once the encoder's pre-skip is trimmed.

const fs = require('fs');
const path = require('path');

const STEREO = path.resolve('tests/audio/fixtures/tone_silence_stereo.opus');
const MONO = path.resolve('tests/audio/fixtures/quiet_loud.opus');

const ctx = new AudioContext();
const sr = ctx.sampleRate;

function rms(a, begin, end, stride, offset) {
    stride = stride || 1; offset = offset || 0;
    let s = 0, n = 0;
    for (let i = begin; i < end; i++) {
        const v = a[i * stride + offset];
        if (v === undefined) break;
        s += v * v; n++;
    }
    return Math.sqrt(s / Math.max(1, n));
}

function zeroCrossHz(a, sr, stride, offset, begin, end) {
    let c = 0, prev = a[begin * stride + offset];
    for (let i = begin + 1; i < end; i++) {
        const v = a[i * stride + offset];
        if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) c++;
        prev = v;
    }
    return c / (2 * ((end - begin) / sr));
}

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

// --- decodeAudioData (memory) -----------------------------------------------
{
    const d = ctx.decodeAudioData(new Uint8Array(fs.readFileSync(STEREO)));
    assert(d, 'decodeAudioData decodes Ogg Opus bytes');
    assert(d.channels === 2, 'stereo Opus has 2 channels, got ' + d.channels);
    assert(d.sampleRate === sr, 'resampled to the engine rate');
    assert(Math.abs(d.numFrames - 3 * sr) <= 4, '3 s exactly after pre-skip trim, got ' + d.numFrames + ' frames vs ' + 3 * sr);
    const n = d.numFrames;
    const toneL = rms(d.samples, Math.floor(0.1 * sr), Math.floor(1.4 * sr), 2, 0);
    const quiet = rms(d.samples, Math.floor(1.6 * sr), Math.floor(2.9 * sr), 2, 0);
    assert(toneL > 0.28 && toneL < 0.42, 'tone half at 0.5 amplitude, rms ' + toneL);
    assert(quiet < 0.01, 'silent half, rms ' + quiet);
    const hzL = zeroCrossHz(d.samples, sr, 2, 0, Math.floor(0.1 * sr), Math.floor(1.4 * sr));
    const hzR = zeroCrossHz(d.samples, sr, 2, 1, Math.floor(0.1 * sr), Math.floor(1.4 * sr));
    assert(Math.abs(hzL - 440) < 20, 'left ~440 Hz, got ' + hzL);
    assert(Math.abs(hzR - 880) < 30, 'right ~880 Hz, got ' + hzR);
    void n;

    const m = ctx.decodeAudioData(new Uint8Array(fs.readFileSync(MONO)));
    assert(m && m.channels === 1, 'mono Opus decodes with 1 channel');
    assert(Math.abs(m.numFrames - 2 * sr) <= 4,'mono: 2 s exactly, got ' + m.numFrames);
}

// --- createClipFromFile (path) ----------------------------------------------
{
    const clip = ctx.createClipFromFile(STEREO);
    assert(clip >= 0, 'createClipFromFile decodes .opus, got ' + clip);
    assert(ctx.getClipChannels(clip) === 2, 'clip is stereo');
    assert(Math.abs(ctx.getClipSampleCount(clip) - 3 * sr) <= 4, 'clip is 3 s, got ' + ctx.getClipSampleCount(clip));
    ctx.deleteClip(clip);
}

// --- createStreamFromFile: plays, knows its length, seeks -------------------
{
    const id = ctx.createStreamFromFile(STEREO);
    assert(typeof id === 'number' && id >= 0, 'createStreamFromFile opens Ogg Opus');
    const dur = ctx.getStreamDuration(id);
    assert(Math.abs(dur - 3) < 1e-3, 'Opus stream duration is 3 s, got ' + dur);
    assert(waitForStats(id, s => s.bufferedFrames >= sr / 4), 'Opus stream prebuffered');

    ctx.startRecording();
    const gulp = Math.ceil(sr * 0.05);
    for (let t = 0; t < 500; t += 50) {
        waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
        sleep(50);
    }
    let rec = ctx.stopRecording();
    const loud = rms(rec, Math.floor(0.1 * sr), Math.floor(0.45 * sr), 2, 0);
    assert(loud > 0.1, 'Opus stream is audible in its tone half, rms ' + loud);

    // Seek into the silent half: the decoder bisects to the page before
    // 2.0 s, pre-rolls 80 ms, and the mixer skips what was buffered.
    ctx.seekPlayback(id, 2.0);
    assert(waitForStats(id, s => s.bufferedFrames >= gulp + 1024 && Math.abs(s.position - 2.0) < 0.05, 5000),
           'stream refilled from 2.0 s, at ' + ctx.getPlaybackPositionSeconds(id));
    ctx.startRecording();
    for (let t = 0; t < 300; t += 50) {
        waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
        sleep(50);
    }
    rec = ctx.stopRecording();
    const after = rms(rec, Math.floor(0.05 * sr), Math.floor(0.25 * sr), 2, 0);
    assert(after < 0.01, 'after seeking to 2.0 s the stream plays the silent half, rms ' + after);
    const pos = ctx.getPlaybackPositionSeconds(id);
    assert(pos > 2.2 && pos < 2.45, 'position follows the seek, ' + pos);

    // And back into the tone: exact-sample trim after the bisection.
    ctx.seekPlayback(id, 1.0);
    assert(waitForStats(id, s => s.bufferedFrames >= gulp + 1024 && Math.abs(s.position - 1.0) < 0.05, 5000),
           'stream refilled from 1.0 s');
    ctx.startRecording();
    for (let t = 0; t < 300; t += 50) {
        waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
        sleep(50);
    }
    rec = ctx.stopRecording();
    const back = rms(rec, Math.floor(0.05 * sr), Math.floor(0.25 * sr), 2, 0);
    assert(back > 0.1, 'seeking back to 1.0 s plays the tone again, rms ' + back);
    ctx.closeStream(id);
}

// --- streamed decode matches the whole-file decode ---------------------------
// A full pass through the stream (seek-free) gives the same frame count as
// decodeAudioData; the file is never resident.
{
    const id = ctx.createStreamFromFile(MONO);
    assert(id >= 0, 'mono Opus streams');
    const fin = (function () {
        const gulp = Math.ceil(sr * 0.1);
        for (let t = 0; t < 3000; t += 100) {
            const st = waitForStats(id, s => s.bufferedFrames >= gulp + 1024 || s.decodedFrames >= 2 * sr - 64, 5000);
            if (st && st.finished) return st;
            sleep(100);
        }
        return waitForStats(id, s => s.finished, 5000);
    })();
    assert(fin && fin.finished, 'mono Opus stream played to the end');
    assert(Math.abs(fin.decodedFrames - 2 * sr) < 64, 'streamed 2 s of frames, got ' + fin.decodedFrames);
    assert(!ctx.isClipPlaying(id), 'finished Opus stream is not playing');
    ctx.closeStream(id);
}

console.log('opus decode tests passed');
