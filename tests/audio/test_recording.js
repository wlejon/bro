// Recording: start/stop, buffer length matches the audio clock exactly.
//
// Headless audio is rendered deterministically: sleep()/advanceTime() drives
// Engine::advanceTime, which calls audioEngine->renderBlock() for exactly the
// virtual-time step. The record tap is a mono mixdown written per rendered
// frame, so the captured length must equal the ctx.currentTime delta times the
// sample rate — an exact audio-clock quantity, not a wall-clock band.

const ctx = new AudioContext();
const sr = ctx.sampleRate;
assert(sr > 0, 'sampleRate positive, got ' + sr);

// Empty recording: start/stop with no audio rendered in between.
{
    ctx.startRecording();
    assert(ctx.recording === true, 'recording flag true after startRecording, got ' + ctx.recording);
    const buf = ctx.stopRecording();
    assert(ctx.recording === false, 'recording flag false after stopRecording, got ' + ctx.recording);
    assert(buf === null, 'no frames rendered while recording => null, got ' + (buf && buf.length));
}

function rms(a) {
    if (!a || !a.length) return 0;
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i] * a[i];
    return Math.sqrt(s / a.length);
}

// Recording length == audio-clock delta while an oscillator plays.
{
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 440;
    osc.connect(ctx.destination);

    const t0 = ctx.currentTime;
    ctx.startRecording();
    osc.start();
    sleep(200);          // virtual time: renders exactly 200 ms of audio
    osc.stop();
    sleep(20);           // render the stop, plus any release tail
    const t1 = ctx.currentTime;
    const buf = ctx.stopRecording();

    assert(buf, 'recording produced a buffer');
    // The record tap is a mono mixdown: one sample per rendered frame.
    const expectedFrames = Math.round((t1 - t0) * sr);
    assert(expectedFrames > 0, 'audio clock advanced during sleep (t1=' + t1 + ' t0=' + t0 + ')');
    console.log('sr:', sr, 'len:', buf.length, 'expectedFrames:', expectedFrames);
    assert(buf.length === expectedFrames,
           'buffer length matches audio clock exactly (' + buf.length + ' vs ' + expectedFrames + ')');

    // Float32 data, finite
    let badCount = 0;
    for (let i = 0; i < buf.length; i++) {
        if (!isFinite(buf[i])) badCount++;
    }
    assert(badCount === 0, 'all samples finite, found ' + badCount + ' bad');

    // The oscillator was audible: the capture is not silence.
    const r = rms(buf);
    console.log('recording rms:', r);
    assert(r > 0.01, 'recorded oscillator is non-silent (rms ' + r + ')');
}

// stopRecording without startRecording: should not crash, returns null or empty
{
    let threw = false;
    let buf;
    try { buf = ctx.stopRecording(); } catch (e) { threw = true; console.log('extra stop threw:', e.message); }
    assert(!threw, 'stopRecording with no active recording does not throw');
    console.log('extra stop returned:', buf === null ? 'null' : (buf && buf.length));
}

// Two-shot recording independence: a silent second recording captures silence.
{
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 440;
    osc.connect(ctx.destination);

    const a0 = ctx.currentTime;
    ctx.startRecording();
    osc.start();
    sleep(100);
    osc.stop();
    // Let the voice fully wind down (stop ramp / release) BEFORE the second
    // recording starts, so recording b contains no deterministic tail.
    sleep(100);
    const a1 = ctx.currentTime;
    const a = ctx.stopRecording();

    const b0 = ctx.currentTime;
    ctx.startRecording();
    sleep(100);
    const b1 = ctx.currentTime;
    const b = ctx.stopRecording();

    assert(a && a.length === Math.round((a1 - a0) * sr),
           'first recording length matches its audio-clock delta');
    assert(b && b.length === Math.round((b1 - b0) * sr),
           'second recording length matches its audio-clock delta');

    const aRms = rms(a), bRms = rms(b);
    console.log('a rms:', aRms, 'b rms (silent):', bRms);
    assert(aRms > 0.01, 'first recording captured the oscillator (rms ' + aRms + ')');
    // Nothing played during b. Headless renders deterministically (no
    // scheduling jitter, no wall-clock bleed), but the master chain leaves a
    // sub-audible residue (~1e-5 RMS: limiter/gain smoothing decay), so
    // "silence" is bounded, not exactly zero. 1e-4 is ~50 dB under the
    // oscillator — real tail ringing or channel leakage lands far above it.
    assert(bRms < 1e-4, 'second recording is silence (rms ' + bRms + ')');
}
