// Bus solo: while any bus is soloed, non-solo buses render silent (parent
// mix AND aux sends); the soloed bus stays audible; mute wins over solo;
// deleting a soloed bus releases solo mode.

const ctx = new AudioContext();

function makeVoiceOnBus(bus) {
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 1.0);
    ctx.setVoiceAttack(v, 0.005);
    ctx.setVoiceSustain(v, 1.0);
    ctx.setVoiceBus(v, bus);
    ctx.startVoice(v, ctx.currentTime);
    return v;
}

function masterPeak() {
    sleep(120);
    return Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));
}

const a = ctx.createBus();   // carries signal
const b = ctx.createBus();   // silent
const v = makeVoiceOnBus(a);

assert(masterPeak() > 0.01, 'voice on bus A audible before solo');

// Solo the SILENT bus: A must vanish from master.
ctx.setBusSolo(b, true);
assert(ctx.getBusSolo(b) === true, 'getBusSolo reflects solo state');
sleep(120);  // let the smoothed tail decay
assert(masterPeak() < 0.05, 'non-solo bus silenced while B soloed');

// Release: A returns.
ctx.setBusSolo(b, false);
assert(ctx.getBusSolo(b) === false, 'solo released');
assert(masterPeak() > 0.01, 'audio returns after solo release');

// Soloing the signal bus keeps it audible.
ctx.setBusSolo(a, true);
assert(masterPeak() > 0.01, 'soloed bus stays audible');

// Mute wins over solo.
ctx.setBusMuted(a, true);
sleep(120);
assert(masterPeak() < 0.05, 'muted+soloed bus is silent');
ctx.setBusMuted(a, false);
ctx.setBusSolo(a, false);

// Deleting a soloed bus releases solo mode.
ctx.setBusSolo(b, true);
sleep(120);
assert(masterPeak() < 0.05, 'A silenced again by soloing B');
ctx.deleteBus(b);
assert(masterPeak() > 0.01, 'deleting the soloed bus restores the mix');

ctx.stopVoice(v, ctx.currentTime);
sleep(30);
ctx.removeVoice(v);
ctx.deleteBus(a);

console.log('test_bus_solo: all assertions passed');
