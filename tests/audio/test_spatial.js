// 3D spatial: listener/source positions, distance models, head model.
// Measured via bus L/R meters (recording is mono mixdown).

const ctx = new AudioContext();

let threw = false;
try {
    ctx.setListenerPosition(0, 0, 0);
    ctx.setListenerOrientation(0, 0, -1, 0, 1, 0);
} catch (e) { threw = true; console.log('listener config threw:', e.message); }
assert(!threw, 'listener configuration does not throw');

function spatialPeaksAt(x, y, z, model) {
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 0.5);
    ctx.setVoiceAttack(v, 0.005);
    ctx.setVoiceSustain(v, 1.0);
    ctx.setVoiceRelease(v, 0.005);
    ctx.setVoiceSpatialEnabled(v, true);
    ctx.setVoiceSpatialPosition(v, x, y, z);
    ctx.setVoiceSpatialRefDistance(v, 1.0);
    ctx.setVoiceSpatialMaxDistance(v, 100.0);
    ctx.setVoiceSpatialRolloff(v, 1.0);
    if (model) ctx.setVoiceSpatialDistanceModel(v, model);
    ctx.startVoice(v, ctx.currentTime);
    sleep(120);
    const l = ctx.getBusPeakL(0);
    const r = ctx.getBusPeakR(0);
    ctx.stopVoice(v, ctx.currentTime);
    sleep(30);
    ctx.removeVoice(v);
    return { l, r };
}

const left  = spatialPeaksAt(-5, 0, -1);
const right = spatialPeaksAt( 5, 0, -1);
console.log('left source bus L/R:', left.l, left.r);
console.log('right source bus L/R:', right.l, right.r);
assert(left.l > left.r, 'source on left -> L > R (L=' + left.l + ' R=' + left.r + ')');
assert(right.r > right.l, 'source on right -> R > L (L=' + right.l + ' R=' + right.r + ')');

// Distance attenuation: far source should be quieter than near
const near = spatialPeaksAt(0, 0, -1);
const far  = spatialPeaksAt(0, 0, -50);
const nearPeak = Math.max(near.l, near.r);
const farPeak  = Math.max(far.l, far.r);
console.log('near peak:', nearPeak, 'far peak:', farPeak);
assert(nearPeak > farPeak, 'near source louder than far (' + nearPeak + ' vs ' + farPeak + ')');

// Distance models accepted
for (const m of ['inverse', 'linear', 'exponential']) {
    const v = ctx.createVoice();
    let t = false;
    try { ctx.setVoiceSpatialDistanceModel(v, m); } catch (e) { t = true; console.log('dist model ' + m + ' threw:', e.message); }
    ctx.removeVoice(v);
    assert(!t, 'distance model ' + m + ' accepted');
}

// Head model setters
threw = false;
try {
    ctx.setHeadModelEnabled(true);
    ctx.setHeadModelIldStrength(0.85);
    ctx.setHeadModelBehindAttenuation(0.45);
    ctx.setHeadModelNearCutoff(18000, 2000);
    ctx.setHeadModelFarCutoffRatio(0.95);
    ctx.setHeadModelElevation(5000, 2000);
    ctx.setHeadModelCutoffRange(200, 20000);
} catch (e) { threw = true; console.log('head model setters threw:', e.message); }
assert(!threw, 'head model setters do not throw');
