// createClipFromFileAsync: promise-based clip loading with decode off the JS
// thread. Success resolves with the clip id; failure rejects with an Error
// carrying the actionable decode message. Top-level await works because the
// headless runner pumps async jobs while the evaluation promise is pending.

const fs = require('fs');
const os = require('os');
const path = require('path');

const ctx = new AudioContext();
const sr = ctx.sampleRate;
const tmp = os.tmpdir();

function rms(a) {
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i] * a[i];
    return Math.sqrt(s / Math.max(1, a.length));
}

// Fixture: 0.5 s 440 Hz tone at the engine rate.
const wavPath = path.join(tmp, 'bro_test_async_tone.wav');
const N = Math.floor(sr * 0.5);
const tone = new Float32Array(N);
for (let i = 0; i < N; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
assert(ctx.saveWav(wavPath, tone, 1, sr), 'wrote async fixture WAV');

// --- success: resolves with a playable clip id -------------------------------
{
    const p = ctx.createClipFromFileAsync(wavPath);
    assert(p instanceof Promise, 'createClipFromFileAsync returns a Promise');
    const clipId = await p;
    assert(typeof clipId === 'number' && clipId >= 0, 'resolved with clip id ' + clipId);
    assert(ctx.getClipChannels(clipId) === 1, 'async-loaded clip is mono');
    assert(ctx.getClipSampleCount(clipId) === N,
           'async-loaded clip has exact frame count, got ' + ctx.getClipSampleCount(clipId));

    // The clip actually plays.
    ctx.startRecording();
    ctx.playClip(clipId, 1.0, false);
    sleep(300);
    const rec = ctx.stopRecording();
    assert(rec && rms(rec) > 0.05, 'async-loaded clip is audible, rms=' + (rec && rms(rec)));
    ctx.deleteClip(clipId);
}

// --- concurrent loads all resolve --------------------------------------------
{
    const ids = await Promise.all([
        ctx.createClipFromFileAsync(wavPath),
        ctx.createClipFromFileAsync(wavPath),
        ctx.createClipFromFileAsync(wavPath),
    ]);
    assert(ids.length === 3 && ids.every(id => id >= 0),
           'three concurrent async loads resolved: ' + JSON.stringify(ids));
    assert(new Set(ids).size === 3, 'each load produced a distinct clip');
    ids.forEach(id => ctx.deleteClip(id));
}

// --- rejection: missing file --------------------------------------------------
{
    let rejected = null;
    try { await ctx.createClipFromFileAsync(path.join(tmp, 'bro_no_such_file.flac')); }
    catch (e) { rejected = e; }
    assert(rejected instanceof Error, 'missing file rejects with an Error');
    assert(String(rejected.message).length > 0, 'rejection has a message: ' + rejected.message);
}

// --- rejection: corrupt file carries the actionable decode error --------------
{
    const badPath = path.join(tmp, 'bro_test_async_bad.ogg');
    // A valid Ogg+Vorbis first packet followed by garbage: sniffs as Vorbis,
    // then setup fails — the reject message must name Vorbis, not just "failed".
    const bad = new Uint8Array(512);
    bad.set([0x4f, 0x67, 0x67, 0x53, 0, 2], 0);                 // "OggS", v0, BOS
    bad[26] = 1; bad[27] = 30;                                   // 1 segment, 30 bytes
    bad.set([0x01, 0x76, 0x6f, 0x72, 0x62, 0x69, 0x73], 28);     // "\x01vorbis"
    for (let i = 35; i < bad.length; i++) bad[i] = (i * 37) & 0xff;
    fs.writeFileSync(badPath, bad);

    let rejected = null;
    try { await ctx.createClipFromFileAsync(badPath); }
    catch (e) { rejected = e; }
    assert(rejected instanceof Error, 'corrupt Vorbis rejects with an Error');
    assert(String(rejected.message).includes('Vorbis'),
           'rejection carries the decode error, got: ' + (rejected && rejected.message));
    fs.unlinkSync(badPath);
}

fs.unlinkSync(wavPath);
console.log('clip async test done');
