// Disk-streamed file playback (createStreamFromFile): a worker thread decodes
// incrementally into a ring the mixer consumes, so the file is never resident.
//
// Pacing: the decode worker is a REAL thread (50 ms wake cadence) while
// sleep() renders virtual time instantly. Tests interleave small virtual
// sleeps with real busy-waits so the worker can keep the ring topped up —
// then deliberately gulp a big virtual chunk to prove the underrun policy
// (silence + counter, playback recovers, audio thread never blocks).

const fs = require('fs');
const os = require('os');
const path = require('path');

const ctx = new AudioContext();
const sr = ctx.sampleRate;
const tmp = os.tmpdir();
const wavPath = path.join(tmp, 'bro_test_stream_tone.wav');

// Real wall-clock wait (the worker runs on its own OS thread and keeps
// decoding while JS spins).
function realWait(ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {}
}

function waitForStats(id, pred, timeoutMs) {
    const t0 = Date.now();
    while (Date.now() - t0 < (timeoutMs || 5000)) {
        const st = ctx.getStreamStats(id);
        if (st && pred(st)) return st;
        realWait(10);
    }
    return null;
}

function rms(a, begin, end) {
    begin = begin || 0; end = end || a.length;
    if (end > a.length) end = a.length;
    let s = 0;
    for (let i = begin; i < end; i++) s += a[i] * a[i];
    return Math.sqrt(s / Math.max(1, end - begin));
}

// 6 s 440 Hz tone at the engine rate (float32 WAV ≈ 1 MB @ 44.1k) — long
// enough that a 1 s ring must refill many times.
const fileSec = 6;
const tone = new Float32Array(sr * fileSec);
for (let i = 0; i < tone.length; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
assert(ctx.saveWav(wavPath, tone, 1, sr), 'wrote streaming fixture WAV');

// --- paced playback: multiple refills, zero underruns, clean finish ---------
{
    const id = ctx.createStreamFromFile(wavPath, { ringFrames: sr });
    assert(typeof id === 'number' && id >= 0, 'createStreamFromFile returns a playback id');

    // Worker prebuffers (~500 ms) then releases playback.
    let st = waitForStats(id, s => s.bufferedFrames >= sr / 4);
    assert(st, 'stream prebuffered, stats: ' + JSON.stringify(ctx.getStreamStats(id)));

    // Volume control applies to a disk stream like any playback.
    ctx.setPlaybackGain(id, 1.0);

    ctx.startRecording();
    // Render the whole file: 0.1 s virtual gulps, real pauses for the worker.
    const steps = (fileSec + 1) * 10;
    for (let i = 0; i < steps; i++) {
        sleep(100);
        realWait(12);
    }
    const rec = ctx.stopRecording();

    st = ctx.getStreamStats(id);
    assert(st, 'stats available after playback');
    console.log('stream stats:', JSON.stringify(st));
    // The whole 6 s file went through a 1 s ring: ≥ 6 refill cycles.
    assert(st.decodedFrames >= sr * fileSec - 8192,
           'whole file decoded through the ring, decoded=' + st.decodedFrames);
    assert(st.playedFrames >= sr * fileSec - 8192,
           'whole file played, played=' + st.playedFrames);
    assert(st.underrunFrames === 0,
           'paced playback never starved, underruns=' + st.underrunFrames);
    const fin = waitForStats(id, s => s.finished);
    assert(fin && fin.finished, 'stream reports finished after EOF + drain');

    // Audio actually flowed, and stopped after the file ended.
    assert(rec && rec.length >= sr * fileSec, 'recording captured the session');
    const rMid = rms(rec, sr, sr * 2);
    assert(rMid > 0.08, 'streamed audio is audible mid-file, rms=' + rMid);
    const rTail = rms(rec, rec.length - Math.floor(sr / 4), rec.length);
    assert(rTail < 0.01, 'silence after the stream finished, rms=' + rTail);

    ctx.closeStream(id);
    assert(ctx.getStreamStats(id) === null, 'stats null after closeStream');
}

// --- underrun: virtual time outruns the worker -> silence + counter ---------
{
    // Tiny ring (0.2 s): one 1.5 s virtual gulp must starve the worker.
    const id = ctx.createStreamFromFile(wavPath, { ringFrames: Math.floor(sr / 5) });
    assert(id >= 0, 'underrun stream created');
    let st = waitForStats(id, s => s.bufferedFrames > 0);
    assert(st, 'underrun stream prebuffered');

    sleep(1500);  // 1.5 s of virtual audio in one real instant
    st = ctx.getStreamStats(id);
    assert(st.underrunFrames > 0,
           'draining faster than the worker refills counts underruns, got ' + st.underrunFrames);

    // Playback recovers once the worker gets wall time again.
    const playedBefore = st.playedFrames;
    st = waitForStats(id, s => s.bufferedFrames >= sr / 20);
    assert(st, 'ring refilled after the stall');
    ctx.startRecording();
    for (let i = 0; i < 5; i++) { sleep(50); realWait(12); }
    const rec = ctx.stopRecording();
    st = ctx.getStreamStats(id);
    assert(st.playedFrames > playedBefore, 'stream kept playing after underrun');
    assert(rms(rec) > 0.03, 'audio resumed after underrun, rms=' + rms(rec));

    ctx.closeStream(id);
}

// --- looping: worker rewinds the decoder at EOF ------------------------------
{
    // 0.4 s file looped for 1.2 s of playback → wraps ≥ 2 times.
    const shortPath = path.join(tmp, 'bro_test_stream_short.wav');
    const shortTone = tone.subarray(0, Math.floor(sr * 0.4));
    assert(ctx.saveWav(shortPath, shortTone, 1, sr), 'wrote short loop WAV');

    const id = ctx.createStreamFromFile(shortPath, { loop: true });
    assert(id >= 0, 'loop stream created');
    let st = waitForStats(id, s => s.bufferedFrames > 0);
    assert(st, 'loop stream prebuffered');

    ctx.startRecording();
    for (let i = 0; i < 12; i++) { sleep(100); realWait(12); }
    const rec = ctx.stopRecording();

    st = ctx.getStreamStats(id);
    assert(st.playedFrames > sr * 0.4 * 2,
           'loop wrapped the file at least twice, played=' + st.playedFrames);
    assert(!st.finished, 'a looping stream never finishes');
    // No dead air at the end of the capture: still playing loudly.
    const rTail = rms(rec, rec.length - Math.floor(sr / 10), rec.length);
    assert(rTail > 0.08, 'loop seam has no dead air, tail rms=' + rTail);

    ctx.closeStream(id);
    fs.unlinkSync(shortPath);
}

// --- error paths --------------------------------------------------------------
{
    let threw = false;
    try { ctx.createStreamFromFile(path.join(tmp, 'bro_no_such_file.mp3')); }
    catch (e) { threw = true; assert(String(e).length > 0, 'error has a message'); }
    assert(threw, 'createStreamFromFile throws on a missing file');

    const junkPath = path.join(tmp, 'bro_test_stream_junk.bin');
    fs.writeFileSync(junkPath, new Uint8Array(256).fill(0x5a));
    threw = false;
    try { ctx.createStreamFromFile(junkPath); }
    catch (e) { threw = true; }
    assert(threw, 'createStreamFromFile throws on unrecognized data');
    fs.unlinkSync(junkPath);

    assert(ctx.getStreamStats(123456) === null, 'stats null for unknown id');
}

// --- teardown race: close immediately after create ---------------------------
{
    const id = ctx.createStreamFromFile(wavPath);
    ctx.closeStream(id);
    ctx.closeStream(id);  // double close is harmless
    assert(ctx.getStreamStats(id) === null, 'closed immediately without issue');
}

// Leave one stream open at exit: engine teardown must stop + join the worker
// (leak/assert failures surface via the exit code under Debug).
const leftOpen = ctx.createStreamFromFile(wavPath);
assert(leftOpen >= 0, 'stream left open for teardown coverage');
sleep(100);

// The open stream still holds the file on Windows — best-effort cleanup only.
try { fs.unlinkSync(wavPath); } catch (e) {}
console.log('stream file test done');
