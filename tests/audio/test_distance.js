// Physical distance through AudioContext (docs/audio-engine-api.js): air
// absorption darkens a source with true distance (bro.ear centroids), the
// propagation delay puts a one-shot's onset at distance / c and keeps a moving
// loop click-free with its Doppler coming from the line, a bus convolution
// reverb reproduces its impulse response, and gain ramps are linear and land
// on time. Headless audio renders exactly the virtual time slept, and the
// record tap is a mono mixdown, one sample per rendered frame.

const ctx = new AudioContext();
const sr = ctx.sampleRate;
ctx.setLimiterEnabled(false);      // its lookahead and gain riding would blur every check
ctx.setHeadModelEnabled(false);

function near(a, b, tol, msg) {
    assert(Math.abs(a - b) <= tol, msg + ' (got ' + a + ', want ' + b + ' +- ' + tol + ')');
}

function record(ms, during) {
    ctx.startRecording();
    if (during) during(); else sleep(ms);
    const buf = ctx.stopRecording();
    assert(buf && buf.length > 0, 'recording captured frames');
    return buf;
}

function firstAbove(buf, thr) {
    for (let i = 0; i < buf.length; i++) if (Math.abs(buf[i]) > thr) return i;
    return -1;
}

function spatial(clip, z, loop) {
    const pb = ctx.playClip(clip, 1.0, loop);
    ctx.setPlaybackSpatialEnabled(pb, true);
    ctx.setPlaybackSpatialRolloff(pb, 0);            // level stays the caller's
    ctx.setPlaybackSpatialMaxDistance(pb, 100000);
    ctx.setPlaybackSpatialPosition(pb, 0, 0, z);
    return pb;
}

// Deterministic white noise.
let seed = 12345;
function noise(n, amp) {
    const a = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        seed = (seed * 1664525 + 1013904223) >>> 0;
        a[i] = amp * (seed / 4294967296 * 2 - 1);
    }
    return a;
}

// ── Air absorption ────────────────────────────────────────────────────────
const noiseClip = ctx.createClip(noise(sr, 0.2), 1);

function centroidAt(units, air) {
    const pb = spatial(noiseClip, -units, true);
    ctx.setPlaybackSpatialAirAbsorption(pb, air);
    sleep(100);                                       // past the first block's smoothing
    const buf = record(500);
    ctx.stopPlayback(pb);
    sleep(20);
    return bro.ear.measure(buf, { sampleRate: sr }).centroidHz;
}

{
    const distances = [5, 50, 150, 300];
    const cs = distances.map(d => centroidAt(d, true));
    console.log('air centroids (Hz) at ' + distances.join('/') + ' m: ' + cs.map(c => c.toFixed(0)).join(' / '));
    for (let i = 1; i < cs.length; i++)
        assert(cs[i] < cs[i - 1], 'centroid falls with distance: ' + distances[i] + ' m');
    assert(cs[3] < 0.5 * cs[0], '300 m is far darker than 5 m');

    // Off: distance does not change the sound (rolloff is 0).
    const off5 = centroidAt(5, false), off300 = centroidAt(300, false);
    near(off300, off5, 0.02 * off5, 'air off leaves the spectrum alone');

    // Strength 0 is no air; strength 2 at 150 m is 300 m.
    ctx.setSpatialAirAbsorptionStrength(0);
    near(centroidAt(300, true), off5, 0.02 * off5, 'strength 0 absorbs nothing');
    ctx.setSpatialAirAbsorptionStrength(2);
    near(centroidAt(150, true), cs[3], 0.03 * cs[3], 'strength 2 doubles the path');
    ctx.setSpatialAirAbsorptionStrength(1);

    // World scale: 150 units of 2 m are 300 m.
    ctx.setSpatialMetresPerUnit(2);
    near(centroidAt(150, true), cs[3], 0.03 * cs[3], 'metres per unit scales the path');
    ctx.setSpatialMetresPerUnit(1);

    // Humid air absorbs less at the top than dry air.
    ctx.setSpatialAirConditions(20, 90);
    const humid = centroidAt(150, true);
    ctx.setSpatialAirConditions(20, 15);
    const dry = centroidAt(150, true);
    ctx.setSpatialAirConditions(20, 50);
    console.log('150 m centroid: 90 % RH ' + humid.toFixed(0) + ' Hz, 15 % RH ' + dry.toFixed(0) + ' Hz');
    assert(humid > dry, 'humidity changes the air');
}

// ── Propagation delay ─────────────────────────────────────────────────────
const burstSamples = new Float32Array(64);
for (let i = 0; i < 32; i++) burstSamples[i] = 0.3;
const burst = ctx.createClip(burstSamples, 1);

function onset(metres, delayOn) {
    const buf = record(0, () => {
        const pb = spatial(burst, -metres, false);
        ctx.setPlaybackSpatialPropagationDelay(pb, delayOn);
        sleep(1200);
    });
    return firstAbove(buf, 1e-3);
}

{
    const base = onset(100, false);
    assert(base >= 0, 'undelayed burst is heard');
    for (const d of [30, 100, 150]) {
        const got = onset(d, true) - base;
        const want = d / 343 * sr;
        console.log('delay ' + d + ' m: onset +' + got + ' frames, expected ' + want.toFixed(1));
        near(got, want, 2, 'onset = distance / c at ' + d + ' m');
    }
    // The 0.5 s cap, then a raised cap.
    near(onset(300, true) - base, 0.5 * sr, 2, 'default cap holds at 300 m');
    ctx.setSpatialMaxPropagationDelay(1.0);
    near(onset(300, true) - base, 300 / 343 * sr, 2, 'raised cap restores 300 m');
    ctx.setSpatialMaxPropagationDelay(0.5);
    // Speed of sound.
    ctx.setSpatialSpeedOfSound(686);
    near(onset(100, true) - base, 100 / 686 * sr, 2, 'speed of sound 686 halves the delay');
    ctx.setSpatialSpeedOfSound(343);

    // A finished one-shot stays alive until its delayed copy has been heard.
    const pb = spatial(burst, -100, false);
    ctx.setPlaybackSpatialPropagationDelay(pb, true);
    sleep(100);
    assert(ctx.isClipPlaying(pb), 'one-shot alive while its sound is in flight');
    sleep(500);
    assert(!ctx.isClipPlaying(pb), 'one-shot ends once its tail has arrived');
}

// A looped tone receding at 100 m/s, moved every 20 ms: no discontinuity
// beyond what the tone itself does, and the pitch ratio is 1 - v/c.
{
    const tone = new Float32Array(sr);
    for (let i = 0; i < sr; i++) tone[i] = 0.25 * Math.sin(2 * Math.PI * 441 * i / sr);
    const toneClip = ctx.createClip(tone, 1);
    ctx.setSpatialMaxPropagationDelay(1.0);
    const v = 100;
    const pb = spatial(toneClip, -10, true);
    ctx.setPlaybackSpatialPropagationDelay(pb, true);
    const ratios = [];
    const buf = record(0, () => {
        for (let step = 0; step < 75; step++) {
            const t = step * 0.02;
            ctx.setPlaybackSpatialPosition(pb, 0, 0, -(10 + v * t));
            sleep(20);
            if (t > 0.5) ratios.push(ctx.getPlaybackDopplerRatio(pb));
        }
    });
    ctx.stopPlayback(pb);
    ctx.setSpatialMaxPropagationDelay(0.5);
    const from = Math.floor(0.2 * sr);
    let peak = 0, maxStep = 0;
    for (let i = from; i < buf.length; i++) {
        peak = Math.max(peak, Math.abs(buf[i]));
        maxStep = Math.max(maxStep, Math.abs(buf[i] - buf[i - 1]));
    }
    const bound = peak * 2 * Math.PI * 441 / sr;
    // Heard pitch from interpolated rising zero crossings over the last 0.8 s.
    let first = -1, last = -1, count = 0;
    for (let i = Math.floor(0.7 * sr); i < buf.length; i++) {
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            const t = i - 1 + buf[i - 1] / (buf[i - 1] - buf[i]);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    }
    const heard = (count - 1) * sr / (last - first) / 441;
    // The reported ratio is the last render block's, which lands at a varying
    // point of the smoothed delay's approach to each 20 ms position step.
    const avg = ratios.reduce((a, b) => a + b, 0) / ratios.length;
    console.log('moving loop: peak ' + peak.toFixed(4) + ', max step ' + maxStep.toFixed(5) +
                ' (sine bound ' + bound.toFixed(5) + '), heard pitch ratio ' + heard.toFixed(4) +
                ', reported ' + avg.toFixed(4) + ' (1 - v/c = ' + (1 - v / 343).toFixed(4) + ')');
    assert(peak > 0.01, 'moving tone is heard');
    assert(maxStep < 1.05 * bound, 'no discontinuity in the moving delay line');
    near(heard, 1 - v / 343, 0.005, 'Doppler falls out of the delay line');
    near(avg, 1 - v / 343, 0.08, 'getPlaybackDopplerRatio reports the line');
}

// ── Convolution reverb ────────────────────────────────────────────────────
{
    const verb = ctx.createBus();
    assert(ctx.getBusConvolutionImpulse(verb) === -1, 'no impulse by default');
    assert(ctx.getBusConvolutionEnabled(verb) === false, 'convolution off by default');
    near(ctx.getBusConvolutionMix(verb), 1, 1e-6, 'mix defaults to fully wet');

    // IR: two taps, 1000 and 3000 frames in.
    const irSamples = new Float32Array(4000);
    irSamples[1000] = 0.5;
    irSamples[3000] = 0.25;
    const ir = ctx.createClip(irSamples, 1);
    assert(ctx.setBusConvolutionImpulse(verb, ir) === true, 'impulse accepted');
    assert(ctx.getBusConvolutionImpulse(verb) === ir, 'impulse getter');
    assert(ctx.setBusConvolutionImpulse(verb, 987654) === false, 'unknown clip refused');
    ctx.deleteClip(ir);                                 // the bus holds its own copy

    const clickSamples = new Float32Array(16);
    clickSamples[0] = 0.8;
    const click = ctx.createClip(clickSamples, 1);
    function clickThrough() {
        return record(0, () => {
            const pb = ctx.playClip(click, 1.0, false);
            ctx.setPlaybackBus(pb, verb);
            sleep(200);
        });
    }
    const dry = clickThrough();                         // convolution still off
    const p0 = firstAbove(dry, 1e-4), v0 = dry[p0];
    assert(p0 >= 0, 'dry click is heard');

    ctx.setBusConvolutionEnabled(verb, true);
    assert(ctx.getBusConvolutionEnabled(verb) === true, 'enabled getter');
    const wet = clickThrough();
    const latency = 256;
    const t1 = p0 + latency + 1000, t2 = p0 + latency + 3000;
    console.log('convolution: taps ' + wet[t1].toFixed(5) + ' / ' + wet[t2].toFixed(5) +
                ' for dry ' + v0.toFixed(5));
    near(wet[t1], 0.5 * v0, 1e-4, 'first tap at 1000 frames + latency');
    near(wet[t2], 0.25 * v0, 1e-4, 'second tap at 3000 frames + latency');
    let stray = 0;
    for (let i = 0; i < wet.length; i++)
        if (i !== t1 && i !== t2) stray = Math.max(stray, Math.abs(wet[i]));
    assert(stray < 1e-4, 'nothing but the two taps (stray ' + stray + ')');

    // Mix 0 is the dry signal; the slot name is accepted by setBusEffectOrder.
    ctx.setBusConvolutionMix(verb, 0);
    sleep(50);
    const mixed = clickThrough();
    near(mixed[p0], v0, 1e-4, 'mix 0 passes the dry click');
    ctx.setBusEffectOrder(verb, ['convolution', 'filter']);
    ctx.setBusConvolutionImpulse(verb, -1);
    assert(ctx.getBusConvolutionImpulse(verb) === -1, 'impulse cleared');
    ctx.deleteBus(verb);
}

// ── Ramps ─────────────────────────────────────────────────────────────────
const dc = ctx.createClip(new Float32Array(4096).fill(0.5), 1);

// Checks buf[s .. s+n) rises linearly from `a` to the plateau after it, and
// stays there: shape to 1e-4 of the plateau.
function isLinearRamp(buf, s, n, a, msg) {
    const b = buf[s + n + 100];
    let worst = 0;
    for (let i = 0; i < n + 200; i++) {
        const want = i < n ? a + (b - a) * (i + 1) / n : b;
        worst = Math.max(worst, Math.abs(buf[s + i] - want));
    }
    console.log(msg + ': ' + n + ' frames ' + a.toFixed(4) + ' -> ' + b.toFixed(4) +
                ', worst deviation ' + worst.toExponential(2));
    assert(worst < 1e-4 * Math.max(Math.abs(a), Math.abs(b)) + 1e-6, msg + ' is linear and lands on time');
}

{
    // A ramp set before the first mix: a fade-in from the creation gain.
    const buf = record(0, () => {
        const pb = ctx.playClip(dc, 0, true);
        ctx.setPlaybackGain(pb, 1, 0.2);
        sleep(400);
        ctx.stopPlayback(pb);
    });
    const s = firstAbove(buf, 0);
    isLinearRamp(buf, s, Math.round(0.2 * sr), 0, 'playback fade-in');
    sleep(50);
}

{
    // A bus gain ramp mid-play, and a playback gain change without one.
    const bus = ctx.createBus();
    ctx.setBusGain(bus, 0.5);
    const pb = ctx.playClip(dc, 1, true);
    ctx.setPlaybackBus(pb, bus);
    sleep(100);
    const before = record(20);
    const level = before[before.length - 1];
    const buf = record(0, () => { ctx.setBusGain(bus, 1.5, 0.3); sleep(500); });
    near(ctx.getBusGain(bus), 1.5, 1e-6, 'getBusGain reads the target');
    isLinearRamp(buf, 0, Math.round(0.3 * sr), level, 'bus gain ramp');

    const top = buf[buf.length - 1];
    const dez = record(0, () => { ctx.setPlaybackGain(pb, 0.5); sleep(200); });
    let maxStep = Math.abs(dez[0] - top);
    for (let i = 1; i < dez.length; i++) maxStep = Math.max(maxStep, Math.abs(dez[i] - dez[i - 1]));
    const change = Math.abs(top - dez[dez.length - 1]);
    console.log('de-zipper: largest step ' + maxStep.toExponential(2) + ' of a ' + change.toFixed(4) + ' change');
    assert(change > 0.05, 'the unramped change happened');
    assert(maxStep < 0.03 * change, 'an unramped change is de-zippered, not a step');
    ctx.stopPlayback(pb);
    ctx.deleteBus(bus);
}

{
    // A send ramp: the direct path is muted, only the send reaches master.
    const dryBus = ctx.createBus();
    ctx.setBusMuted(dryBus, true);
    const ret = ctx.createBus();
    const pb = ctx.playClip(dc, 1, true);
    ctx.setPlaybackBus(pb, dryBus);
    ctx.setPlaybackSend(pb, ret, 0);
    sleep(100);
    const buf = record(0, () => { ctx.setPlaybackSend(pb, ret, 0.8, 0.25); sleep(400); });
    isLinearRamp(buf, 0, Math.round(0.25 * sr), 0, 'send ramp');
    ctx.stopPlayback(pb);
    ctx.deleteBus(ret);
    ctx.deleteBus(dryBus);
}

console.log('test_distance: ok');
