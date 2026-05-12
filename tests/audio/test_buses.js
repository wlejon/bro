// Mix bus hierarchy: create/delete, gain, pan, mute, routing, aux sends.

const ctx = new AudioContext();

function rms(buf) {
    if (!buf || !buf.length) return 0;
    let s = 0;
    for (let i = 0; i < buf.length; i++) s += buf[i]*buf[i];
    return Math.sqrt(s / buf.length);
}

function playToneOnBus(busId, ms) {
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 0.3);
    ctx.setVoiceAttack(v, 0.005);
    ctx.setVoiceSustain(v, 1.0);
    ctx.setVoiceRelease(v, 0.01);
    if (busId >= 0) ctx.setVoiceBus(v, busId);
    ctx.startVoice(v, ctx.currentTime);
    sleep(ms);
    ctx.stopVoice(v, ctx.currentTime);
    sleep(20);
    ctx.removeVoice(v);
}

// Create a bus
const bus = ctx.createBus();
console.log('createBus returned:', bus);
assert(typeof bus === 'number' && bus >= 0, 'createBus returns a non-negative id, got ' + bus); // BUG: create-bus

// Baseline: unmuted bus produces audio
ctx.setBusGain(bus, 1.0);
ctx.setBusMuted(bus, false);
ctx.startRecording();
playToneOnBus(bus, 120);
const unmutedRms = rms(ctx.stopRecording());

// Muted bus should be silent
ctx.setBusMuted(bus, true);
ctx.startRecording();
playToneOnBus(bus, 120);
const mutedRms = rms(ctx.stopRecording());

console.log('bus unmuted rms:', unmutedRms, 'muted rms:', mutedRms);
assert(unmutedRms > 0.001, 'unmuted bus is audible'); // BUG: bus-routing
assert(mutedRms < unmutedRms * 0.05, 'mute really silences bus (muted=' + mutedRms + ', unmuted=' + unmutedRms + ')'); // BUG: bus-mute

// Gain affects level proportionally
ctx.setBusMuted(bus, false);
ctx.setBusGain(bus, 0.25);
ctx.startRecording();
playToneOnBus(bus, 120);
const lowGainRms = rms(ctx.stopRecording());
console.log('bus gain=0.25 rms:', lowGainRms);
assert(lowGainRms < unmutedRms * 0.5, 'lower bus gain produces lower level (' + lowGainRms + ' < 0.5*' + unmutedRms + ')'); // BUG: bus-gain

// Metering API returns numbers
ctx.setBusGain(bus, 1.0);
const pL = ctx.getBusPeakL(bus);
const pR = ctx.getBusPeakR(bus);
const rL = ctx.getBusRmsL(bus);
const rR = ctx.getBusRmsR(bus);
console.log('meter L/R peak:', pL, pR, 'rms:', rL, rR);
assert(typeof pL === 'number' && !isNaN(pL), 'getBusPeakL returns number, got ' + pL); // BUG: bus-meter
assert(typeof rR === 'number' && !isNaN(rR), 'getBusRmsR returns number, got ' + rR); // BUG: bus-meter

// deleteBus does not throw on valid id
let threw = false;
try { ctx.deleteBus(bus); } catch (e) { threw = true; console.log('deleteBus threw:', e.message); }
assert(!threw, 'deleteBus does not throw on valid id'); // BUG: delete-bus
