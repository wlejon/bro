// Doppler: source/listener velocities + global factor, applied to
// spatialized clip playbacks (resample-rate compose) and voices (pitch).
// Ratios verified via the getPlaybackDopplerRatio/getVoiceDopplerRatio
// introspection accessors; c = 343, ratio clamped to [0.5, 2.0].

const ctx = new AudioContext();
const sr = ctx.sampleRate;

assert(typeof ctx.dopplerFactor === 'number', 'dopplerFactor readable');
assert(ctx.dopplerFactor === 1, 'dopplerFactor defaults to 1');

const tone = new Float32Array(sr);
for (let i = 0; i < tone.length; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
const clip = ctx.createClip(tone, 1);

function spatialPlayback(px, py, pz, vx, vy, vz) {
    const id = ctx.playClip(clip, 1.0, true);
    ctx.setPlaybackSpatialEnabled(id, true);
    ctx.setPlaybackSpatialPosition(id, px, py, pz);
    ctx.setPlaybackSpatialVelocity(id, vx, vy, vz);
    return id;
}

function near(a, b, tol, msg) {
    assert(Math.abs(a - b) < tol, msg + ' (got ' + a + ', want ~' + b + ')');
}

// Approaching source, 10 units out at 10 u/s: ratio = 343 / 333.
{
    const id = spatialPlayback(0, 0, -10, 0, 0, 10);
    sleep(50);
    near(ctx.getPlaybackDopplerRatio(id), 343 / 333, 0.005, 'approach raises ratio');
    ctx.stopPlayback(id);
}

// Receding source: ratio = 343 / 353 < 1.
{
    const id = spatialPlayback(0, 0, -10, 0, 0, -10);
    sleep(50);
    near(ctx.getPlaybackDopplerRatio(id), 343 / 353, 0.005, 'recede lowers ratio');
    ctx.stopPlayback(id);
}

// Listener velocity contributes: listener moving toward a static source.
{
    const id = spatialPlayback(0, 0, -10, 0, 0, 0);
    ctx.setListenerVelocity(0, 0, -5);
    sleep(50);
    near(ctx.getPlaybackDopplerRatio(id), 348 / 343, 0.005, 'listener motion shifts ratio');
    ctx.setListenerVelocity(0, 0, 0);
    ctx.stopPlayback(id);
}

// Factor 0 disables.
{
    const id = spatialPlayback(0, 0, -10, 0, 0, 10);
    ctx.dopplerFactor = 0;
    sleep(50);
    near(ctx.getPlaybackDopplerRatio(id), 1.0, 1e-6, 'factor 0 disables Doppler');
    ctx.dopplerFactor = 1;
    ctx.stopPlayback(id);
}

// Extreme approach clamps to 2.0 (one octave up).
{
    const id = spatialPlayback(0, 0, -10, 0, 0, 5000);
    sleep(50);
    near(ctx.getPlaybackDopplerRatio(id), 2.0, 1e-6, 'ratio clamps at 2.0');
    ctx.stopPlayback(id);
}

// Voices: spatialized voice with approach velocity reports ratio > 1.
{
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 0.5);
    ctx.setVoiceSpatialEnabled(v, true);
    ctx.setVoiceSpatialPosition(v, 0, 0, -10);
    ctx.setVoiceSpatialVelocity(v, 0, 0, 10);
    ctx.startVoice(v, ctx.currentTime);
    sleep(50);
    near(ctx.getVoiceDopplerRatio(v), 343 / 333, 0.005, 'voice Doppler ratio tracks motion');
    ctx.stopVoice(v, ctx.currentTime);
    sleep(30);
    ctx.removeVoice(v);
}

console.log('test_doppler: all assertions passed');
