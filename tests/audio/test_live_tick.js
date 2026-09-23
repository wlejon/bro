// broaudio's per-frame host tick (tickAsyncJobs, pumped by the engine's frame
// pump): scheduled AudioParam automation reaches a source that is already
// playing, and a finished AudioBufferSourceNode fires `onended`. Neither
// happens unless the host ticks broaudio every frame; nothing here calls
// ctx.renderBlock, so the only thing driving them is the engine's frame loop
// under advanceTime/sleep.

const ctx = new AudioContext();
const sr = ctx.sampleRate;

function peakHz() {
    const bins = 1024;
    const spec = ctx.getSpectrum(bins);
    let maxBin = 0, maxVal = -Infinity;
    for (let i = 0; i < bins; i++) if (spec[i] > maxVal) { maxVal = spec[i]; maxBin = i; }
    return maxBin * (sr / 2) / bins;
}

// --- automation on a playing oscillator --------------------------------------
{
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 500;
    osc.connect(ctx.destination);
    osc.start();
    sleep(250);
    const before = peakHz();
    assert(Math.abs(before - 500) < 100, 'oscillator starts at 500 Hz, got ' + before);

    const t = ctx.currentTime;
    osc.frequency.setValueAtTime(500, t);
    osc.frequency.linearRampToValueAtTime(2000, t + 0.1);
    sleep(400);
    const after = peakHz();
    assert(Math.abs(after - 2000) < 150, 'the scheduled ramp reached the playing oscillator, got ' + after);
    // As in Web Audio, reading .value while a timeline exists reports the
    // automated value at currentTime (the ramp has ended at 2000).
    assert(Math.abs(osc.frequency.value - 2000) < 1e-3,
        'param.value reads the automation\'s current value, got ' + osc.frequency.value);
    osc.stop();
    sleep(50);
}

// --- onended from the frame tick ----------------------------------------------
{
    const len = Math.round(sr * 0.05);
    const buf = ctx.createBuffer(1, len, sr);
    const data = buf.getChannelData(0);
    for (let i = 0; i < len; i++) data[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);

    const src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(ctx.destination);
    let ended = 0, sawThis = false;
    src.onended = function (e) { ended++; sawThis = this === src && e.type === 'ended'; };
    src.start();
    advanceTime(16);
    assert(ended === 0, 'no onended while the 50 ms buffer is still playing');
    for (let i = 0; i < 10; i++) advanceTime(16);
    assert(ended === 1, 'onended fired once from the frame tick, got ' + ended);
    assert(sawThis, 'onended ran with this = the source and an "ended" event');
}

console.log('test_live_tick: OK');
