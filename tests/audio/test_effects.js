// Per-bus effects: each documented effect should measurably alter the signal.

const ctx = new AudioContext();

function rms(buf) {
    if (!buf || !buf.length) return 0;
    let s = 0;
    for (let i = 0; i < buf.length; i++) s += buf[i]*buf[i];
    return Math.sqrt(s / buf.length);
}

function recordTone(setup, durMs) {
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 0.3);
    ctx.setVoiceAttack(v, 0.005);
    ctx.setVoiceSustain(v, 1.0);
    ctx.setVoiceRelease(v, 0.01);
    if (setup) setup(v);
    ctx.startRecording();
    ctx.startVoice(v, ctx.currentTime);
    sleep(durMs);
    ctx.stopVoice(v, ctx.currentTime);
    sleep(40);
    const buf = ctx.stopRecording();
    ctx.removeVoice(v);
    return buf;
}

const bus = ctx.createBus();
function onBus(v) { ctx.setVoiceBus(v, bus); }

// Reset all effects off on this bus
function resetBus() {
    ctx.setBusDelayEnabled(bus, false);
    ctx.setBusReverbEnabled(bus, false);
    ctx.setBusChorusEnabled(bus, false);
    ctx.setBusCompressorEnabled(bus, false);
    ctx.setBusEqEnabled(bus, false);
    ctx.setBusDistortionEnabled(bus, false);
}

// Baseline (dry)
resetBus();
const dryRms = rms(recordTone(onBus, 150));
console.log('dry rms:', dryRms);
assert(dryRms > 0.001, 'baseline tone is audible'); // BUG: bus-baseline

function expectDifferent(name, modify, tolerance = 0.0005) {
    resetBus();
    modify();
    const wetBuf = recordTone(onBus, 200);
    const wetRms = rms(wetBuf);
    console.log(name, 'wet rms:', wetRms, 'dry rms:', dryRms, 'delta:', Math.abs(wetRms - dryRms));
    assert(Math.abs(wetRms - dryRms) > tolerance,
           name + ' alters signal (wet=' + wetRms + ', dry=' + dryRms + ')'); // BUG: effect-noop
}

expectDifferent('biquad-filter', () => {
    const slot = ctx.allocateBusFilterSlot(bus);
    console.log('filter slot:', slot);
    assert(typeof slot === 'number' && slot >= 0, 'allocateBusFilterSlot returns slot, got ' + slot);
    ctx.setBusFilterType(bus, slot, 'lowpass');
    ctx.setBusFilterFrequency(bus, slot, 200);   // strongly cuts a 440Hz sine
    ctx.setBusFilterQ(bus, slot, 1.0);
    ctx.setBusFilterEnabled(bus, slot, true);
});

expectDifferent('delay', () => {
    ctx.setBusDelayEnabled(bus, true);
    ctx.setBusDelayTime(bus, 0.05);
    ctx.setBusDelayFeedback(bus, 0.6);
    ctx.setBusDelayMix(bus, 0.7);
});

expectDifferent('reverb', () => {
    ctx.setBusReverbEnabled(bus, true);
    ctx.setBusReverbRoomSize(bus, 0.9);
    ctx.setBusReverbDamping(bus, 0.5);
    ctx.setBusReverbMix(bus, 0.8);
});

expectDifferent('chorus', () => {
    ctx.setBusChorusEnabled(bus, true);
    ctx.setBusChorusRate(bus, 1.5);
    ctx.setBusChorusDepth(bus, 0.5);
    ctx.setBusChorusMix(bus, 0.8);
});

expectDifferent('compressor', () => {
    ctx.setBusCompressorEnabled(bus, true);
    ctx.setBusCompressorThreshold(bus, -40);
    ctx.setBusCompressorRatio(bus, 8);
    ctx.setBusCompressorAttack(bus, 1);
    ctx.setBusCompressorRelease(bus, 50);
});

expectDifferent('eq', () => {
    ctx.setBusEqEnabled(bus, true);
    ctx.setBusEqBandGain(bus, 3, -24);  // ~1 kHz cut, dry is 440 Hz; partial impact
    ctx.setBusEqBandGain(bus, 1, -24);  // ~170 Hz cut
});

for (const mode of ['softclip', 'hardclip', 'foldback', 'bitcrush']) {
    expectDifferent('distortion-' + mode, () => {
        ctx.setBusDistortionEnabled(bus, true);
        ctx.setBusDistortionMode(bus, mode);
        ctx.setBusDistortionDrive(bus, 8.0);
        ctx.setBusDistortionMix(bus, 1.0);
        if (mode === 'bitcrush') {
            ctx.setBusDistortionCrushBits(bus, 3);
            ctx.setBusDistortionCrushRate(bus, 0.1);
        }
    });
}
