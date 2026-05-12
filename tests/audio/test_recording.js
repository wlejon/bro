// Recording: start/stop, buffer length matches elapsed audio time.

const ctx = new AudioContext();
const sr = ctx.sampleRate;

// Empty recording
{
    ctx.startRecording();
    assert(ctx.recording === true, 'recording flag true after startRecording, got ' + ctx.recording); // BUG: rec-flag
    const buf = ctx.stopRecording();
    assert(ctx.recording === false, 'recording flag false after stopRecording, got ' + ctx.recording); // BUG: rec-flag-stop
    // Returns null on nothing captured OR a small float32 array
    console.log('empty recording:', buf === null ? 'null' : buf.length);
}

// Sleep duration → expected sample count (stereo interleaved)
{
    const oscMs = 200;
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 440;
    osc.connect(ctx.destination);
    ctx.startRecording();
    osc.start();
    sleep(oscMs);
    osc.stop();
    sleep(20);
    const buf = ctx.stopRecording();
    assert(buf, 'recording produced a buffer'); // BUG: rec-empty
    const expectedFrames = Math.floor(sr * (oscMs + 20) / 1000);
    // Buffer can be mono (length==frames) or stereo interleaved (length==frames*2).
    // Just sanity-check it's the right order of magnitude.
    console.log('sr:', sr, 'len:', buf.length, 'expectedFrames:', expectedFrames);
    assert(buf.length >= expectedFrames * 0.4, 'buffer length >= ~0.4x expected frames (' + buf.length + ' vs ' + expectedFrames + ')'); // BUG: rec-length-low
    assert(buf.length <= expectedFrames * 4,   'buffer length <= ~4x expected frames (stereo+slack)'); // BUG: rec-length-high

    // Float32 data, finite
    let badCount = 0;
    for (let i = 0; i < buf.length; i++) {
        if (!isFinite(buf[i])) badCount++;
    }
    assert(badCount === 0, 'all samples finite, found ' + badCount + ' bad'); // BUG: rec-nan
}

// stopRecording without startRecording: should not crash, returns null or empty
{
    let threw = false;
    let buf;
    try { buf = ctx.stopRecording(); } catch (e) { threw = true; console.log('extra stop threw:', e.message); }
    assert(!threw, 'stopRecording with no active recording does not throw'); // BUG: rec-stop-noop
    console.log('extra stop returned:', buf === null ? 'null' : (buf && buf.length));
}

// Two-shot recording independence
{
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 440;
    osc.connect(ctx.destination);
    ctx.startRecording();
    osc.start();
    sleep(100);
    osc.stop();
    sleep(10);
    const a = ctx.stopRecording();

    ctx.startRecording();
    sleep(100);
    const b = ctx.stopRecording();

    assert(a && a.length > 0, 'first recording captured'); // BUG: rec-first
    assert(b !== null && b !== undefined, 'second recording returned a buffer (silent or otherwise), got ' + b); // BUG: rec-second
    let aRms = 0, bRms = 0;
    if (a) { for (let i = 0; i < a.length; i++) aRms += a[i]*a[i]; aRms = Math.sqrt(aRms/a.length); }
    if (b && b.length) { for (let i = 0; i < b.length; i++) bRms += b[i]*b[i]; bRms = Math.sqrt(bRms/b.length); }
    console.log('a rms:', aRms, 'b rms (silent):', bRms);
    assert(bRms < aRms * 0.2, 'second (silent) recording quieter than first'); // BUG: rec-isolation
}
