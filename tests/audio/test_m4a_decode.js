// M4A (AAC in MP4): decodeAudioData, createClipFromFile and createStreamFromFile
// on a real file, through the platform's AAC decoder (Media Foundation on
// Windows, AudioToolbox on macOS):
//   quiet_loud.m4a  2 s, 44.1 kHz mono, silent 1 s then 440 Hz at 0.6
//                   (ffmpeg AAC-LC 64 kb/s from quiet_loud.flac, with tags)
// The edit list trims the encoder's priming, so the length is exact: 2 s.
// On Linux there is no AAC decoder in bro, and each entry point must say so.

const fs = require('fs');
const os = require('os');
const path = require('path');

const FILE = path.resolve('tests/audio/fixtures/quiet_loud.m4a');
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

function zeroCrossHz(a, rate, begin, end) {
    let c = 0, prev = a[begin];
    for (let i = begin + 1; i < end; i++) {
        const v = a[i];
        if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) c++;
        prev = v;
    }
    return c / (2 * ((end - begin) / rate));
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

if (os.platform() === 'linux') {
    let msg = '';
    try {
        const d = ctx.decodeAudioData(new Uint8Array(fs.readFileSync(FILE)));
        msg = d ? 'decoded' : '';
    } catch (e) { msg = String(e && e.message || e); }
    assert(/platform AAC decoder/.test(msg), 'Linux: decodeAudioData says M4A needs a platform AAC decoder: ' + msg);
    let err = '';
    try { ctx.createStreamFromFile(FILE); } catch (e) { err = String(e && e.message || e); }
    assert(/platform AAC decoder/.test(err), 'Linux: createStreamFromFile says the same: ' + err);
    console.log('m4a decode: no platform AAC decoder on Linux, as documented');
} else {
    // --- decodeAudioData (memory) -------------------------------------------
    {
        const d = ctx.decodeAudioData(new Uint8Array(fs.readFileSync(FILE)));
        assert(d, 'decodeAudioData decodes M4A bytes');
        assert(d.channels === 1, 'mono M4A has 1 channel, got ' + d.channels);
        assert(d.sampleRate === sr, 'resampled to the engine rate');
        assert(Math.abs(d.numFrames - 2 * sr) <= 4, '2 s exactly after the priming trim, got ' + d.numFrames + ' vs ' + 2 * sr);
        const quiet = rms(d.samples, Math.floor(0.05 * sr), Math.floor(0.95 * sr));
        const loud = rms(d.samples, Math.floor(1.05 * sr), Math.floor(1.95 * sr));
        assert(quiet < 0.01, 'silent first second, rms ' + quiet);
        assert(loud > 0.38 && loud < 0.47, 'tone at 0.6 amplitude in the second, rms ' + loud);
        const hz = zeroCrossHz(d.samples, sr, Math.floor(1.05 * sr), Math.floor(1.95 * sr));
        assert(Math.abs(hz - 440) < 10, '~440 Hz, got ' + hz);
        // The tone starts where the source's does (1.0 s): the priming and the
        // decoder's own latency are both accounted for.
        let onset = Math.floor(0.9 * sr);
        while (onset < d.numFrames && Math.abs(d.samples[onset]) < 0.1) onset++;
        assert(Math.abs(onset / sr - 1.0) < 0.005, 'the tone starts at 1.0 s, at ' + (onset / sr).toFixed(4));
    }

    // --- createClipFromFile (path) -------------------------------------------
    {
        const clip = ctx.createClipFromFile(FILE);
        assert(clip >= 0, 'createClipFromFile decodes .m4a, got ' + clip);
        assert(ctx.getClipChannels(clip) === 1, 'clip is mono');
        assert(Math.abs(ctx.getClipSampleCount(clip) - 2 * sr) <= 4, 'clip is 2 s, got ' + ctx.getClipSampleCount(clip));
        ctx.deleteClip(clip);
    }

    // --- createStreamFromFile: plays, knows its length, seeks ---------------
    {
        const id = ctx.createStreamFromFile(FILE);
        assert(typeof id === 'number' && id >= 0, 'createStreamFromFile opens M4A');
        const dur = ctx.getStreamDuration(id);
        assert(Math.abs(dur - 2) < 1e-3, 'M4A stream duration is 2 s, got ' + dur);
        const gulp = Math.ceil(sr * 0.05);

        // Into the tone.
        ctx.seekPlayback(id, 1.2);
        assert(waitForStats(id, s => s.bufferedFrames >= gulp + 1024 && Math.abs(s.position - 1.2) < 0.05, 5000),
               'stream refilled from 1.2 s, at ' + ctx.getPlaybackPositionSeconds(id));
        ctx.startRecording();
        for (let t = 0; t < 300; t += 50) {
            waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
            sleep(50);
        }
        let rec = ctx.stopRecording();
        const loud = rms(rec, Math.floor(0.05 * sr), Math.floor(0.25 * sr), 2, 0);
        assert(loud > 0.1, 'after seeking to 1.2 s the stream plays the tone, rms ' + loud);
        const pos = ctx.getPlaybackPositionSeconds(id);
        assert(pos > 1.4 && pos < 1.65, 'position follows the seek, ' + pos);

        // Back into the silence.
        ctx.seekPlayback(id, 0.1);
        assert(waitForStats(id, s => s.bufferedFrames >= gulp + 1024 && Math.abs(s.position - 0.1) < 0.05, 5000),
               'stream refilled from 0.1 s');
        ctx.startRecording();
        for (let t = 0; t < 300; t += 50) {
            waitForStats(id, s => s.bufferedFrames >= gulp + 1024, 5000);
            sleep(50);
        }
        rec = ctx.stopRecording();
        const quiet = rms(rec, Math.floor(0.05 * sr), Math.floor(0.25 * sr), 2, 0);
        assert(quiet < 0.01, 'seeking back to 0.1 s plays the silence, rms ' + quiet);
        ctx.closeStream(id);
    }

    // --- a full streamed pass gives the whole file's frames ------------------
    {
        const id = ctx.createStreamFromFile(FILE);
        const fin = (function () {
            const gulp = Math.ceil(sr * 0.1);
            for (let t = 0; t < 3000; t += 100) {
                const st = waitForStats(id, s => s.bufferedFrames >= gulp + 1024 || s.decodedFrames >= 2 * sr - 64, 5000);
                if (st && st.finished) return st;
                sleep(100);
            }
            return waitForStats(id, s => s.finished, 5000);
        })();
        assert(fin && fin.finished, 'the M4A stream played to the end');
        assert(Math.abs(fin.decodedFrames - 2 * sr) < 64, 'streamed 2 s of frames, got ' + fin.decodedFrames);
        ctx.closeStream(id);
    }
    console.log('m4a decode tests passed');
}
