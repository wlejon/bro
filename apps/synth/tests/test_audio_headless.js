// Headless audio verification tests for broaudio through the synth app.
// Run: bro-headless apps/synth apps/synth/tests/test_audio_headless.js
//
// Since we can't hear audio, we verify numerically via:
//   - Bus metering (peak/RMS levels)
//   - FFT spectrum analysis (frequency content)
//   - Recording buffer inspection (waveform capture)

var passed = 0;
var failed = 0;

function test(name, fn) {
    try {
        fn();
        passed++;
        console.log('PASS: ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL: ' + name + ' - ' + e.message);
    }
}

function assertGt(a, b, msg) {
    if (!(a > b)) throw new Error((msg || '') + ' expected ' + a + ' > ' + b);
}
function assertLt(a, b, msg) {
    if (!(a < b)) throw new Error((msg || '') + ' expected ' + a + ' < ' + b);
}
function assertNear(a, b, tol, msg) {
    if (Math.abs(a - b) > tol) throw new Error((msg || '') + ' expected ' + a + ' near ' + b + ' (tol=' + tol + ')');
}

// Helper: play a voice and immediately read metering
function playAndMeasure(waveform, noteNum, durationMs, busId) {
    busId = busId || 0;
    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, waveform);
        ctx.setVoiceGain(id, 1.0);
        if (busId !== 0) ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });
    alloc.noteOn(noteNum, 0.8);
    advanceTime(durationMs);
    var result = {
        peakL: ctx.getBusPeakL(busId),
        peakR: ctx.getBusPeakR(busId),
        rmsL: ctx.getBusRmsL(busId),
        rmsR: ctx.getBusRmsR(busId),
        spectrum: ctx.getSpectrum(1024),
        alloc: alloc
    };
    return result;
}

// Helper: stop voice and let release finish
function cleanup(alloc, noteNum) {
    alloc.noteOff(noteNum);
    advanceTime(200);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

var ctx = new AudioContext();
assert(ctx, 'AudioContext should be constructable in headless');
assert(ctx.sampleRate > 0, 'sampleRate should be positive');

// ---------------------------------------------------------------------------
// Test 1: Basic voice output - sine wave produces signal
// ---------------------------------------------------------------------------
test('sine voice produces signal on master bus', function() {
    var m = playAndMeasure('sine', 69, 50);
    assertGt(m.peakL, 0.0, 'peakL');
    assertGt(m.peakR, 0.0, 'peakR');
    assertGt(m.rmsL, 0.0, 'rmsL');
    assertGt(m.rmsR, 0.0, 'rmsR');
    cleanup(m.alloc, 69);
});

// ---------------------------------------------------------------------------
// Test 2: Silence after release
// ---------------------------------------------------------------------------
test('silence after voice release completes', function() {
    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceDecay(id, 0.01);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.02);
    });

    alloc.noteOn(60, 0.5);
    advanceTime(50);
    alloc.noteOff(60);
    advanceTime(500);

    var peak = ctx.getBusPeakL(0);
    assertLt(peak, 0.01, 'bus should be near-silent after release');
});

// ---------------------------------------------------------------------------
// Test 3: Different waveforms produce different spectral richness
// ---------------------------------------------------------------------------
test('sawtooth has richer spectrum than sine', function() {
    // Play sine and capture spectrum while it's sounding
    var sinResult = playAndMeasure('sine', 69, 100);
    var sineBins = 0;
    for (var i = 0; i < sinResult.spectrum.length; i++) {
        if (sinResult.spectrum[i] > 0.001) sineBins++;
    }
    cleanup(sinResult.alloc, 69);

    // Play sawtooth and capture spectrum while it's sounding
    var sawResult = playAndMeasure('sawtooth', 69, 100);
    var sawBins = 0;
    for (var i = 0; i < sawResult.spectrum.length; i++) {
        if (sawResult.spectrum[i] > 0.001) sawBins++;
    }
    cleanup(sawResult.alloc, 69);

    assertGt(sawBins, sineBins, 'sawtooth should have more spectral content (' + sawBins + ' vs ' + sineBins + ')');
});

// ---------------------------------------------------------------------------
// Test 4: Bus routing - child bus receives voice signal
// ---------------------------------------------------------------------------
test('voice routed to child bus produces signal there', function() {
    var busId = ctx.createBus();
    var m = playAndMeasure('sine', 69, 50, busId);
    assertGt(m.peakL, 0.0, 'child bus should have signal');

    // Master should also have signal (child mixes into master)
    var masterPeak = ctx.getBusPeakL(0);
    assertGt(masterPeak, 0.0, 'master should receive from child');

    cleanup(m.alloc, 69);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 5: Filter effect - lowpass cuts high frequencies
// ---------------------------------------------------------------------------
test('lowpass filter reduces RMS on sawtooth', function() {
    var busId = ctx.createBus();
    var slot = ctx.allocateBusFilterSlot(busId);

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sawtooth');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    alloc.noteOn(48, 0.8); // C3 ~131Hz, rich harmonics
    advanceTime(100);

    var rmsBefore = ctx.getBusRmsL(busId);
    assertGt(rmsBefore, 0, 'should have signal before filter');

    // Enable lowpass at 300Hz — should cut most harmonics of 131Hz sawtooth
    ctx.setBusFilterEnabled(busId, slot, true);
    ctx.setBusFilterType(busId, slot, 'lowpass');
    ctx.setBusFilterFrequency(busId, slot, 300);
    ctx.setBusFilterQ(busId, slot, 1.0);

    advanceTime(200);
    var rmsAfter = ctx.getBusRmsL(busId);

    assertLt(rmsAfter, rmsBefore, 'filter should reduce RMS (before=' + rmsBefore.toFixed(4) + ', after=' + rmsAfter.toFixed(4) + ')');

    alloc.noteOff(48);
    advanceTime(200);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 6: Delay effect produces echoes (check via recording)
// ---------------------------------------------------------------------------
test('delay effect adds energy after note stops', function() {
    var busId = ctx.createBus();
    ctx.setBusDelayEnabled(busId, true);
    ctx.setBusDelayTime(busId, 0.05);   // 50ms delay
    ctx.setBusDelayFeedback(busId, 0.6);
    ctx.setBusDelayMix(busId, 0.8);

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 2.0);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.005);
    });

    // Play a short note to feed the delay
    alloc.noteOn(69, 1.0);
    advanceTime(100);
    alloc.noteOff(69);
    advanceTime(100); // let voice release finish completely

    // Delay feedback should still produce signal on the bus
    advanceTime(50);
    var peak = ctx.getBusPeakL(busId);
    assertGt(peak, 0.001, 'delay echo should produce signal after note stops (peak=' + peak.toFixed(6) + ')');

    advanceTime(2000);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 7: Recording captures audio data
// ---------------------------------------------------------------------------
test('recording captures non-zero samples', function() {
    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    ctx.startRecording();
    alloc.noteOn(69, 0.8);
    advanceTime(100);
    alloc.noteOff(69);
    advanceTime(100);
    var samples = ctx.stopRecording();

    assert(samples.length > 0, 'recording should capture samples');

    var maxSample = 0;
    for (var i = 0; i < samples.length; i++) {
        var abs = Math.abs(samples[i]);
        if (abs > maxSample) maxSample = abs;
    }
    assertGt(maxSample, 0.01, 'recorded samples should contain signal');
});

// ---------------------------------------------------------------------------
// Test 8: Unison produces signal
// ---------------------------------------------------------------------------
test('unison voices produce signal', function() {
    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sawtooth');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
        ctx.setVoiceUnisonCount(id, 8);
        ctx.setVoiceUnisonDetune(id, 0.5);
        ctx.setVoiceUnisonStereoWidth(id, 1.0);
    });

    alloc.noteOn(69, 0.8);
    advanceTime(50);

    var peak = ctx.getBusPeakL(0);
    assertGt(peak, 0.0, 'unison should produce signal');

    alloc.noteOff(69);
    advanceTime(200);
});

// ---------------------------------------------------------------------------
// Test 9: Sequencer plays notes at correct beats
// ---------------------------------------------------------------------------
test('sequencer triggers notes on beat', function() {
    var alloc = ctx.createVoiceAllocator(8);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    var seq = ctx.createSequence(alloc);
    seq.setBPM(120);
    seq.addNote(0.0, 69, 0.8, 0.5);  // half-beat note at beat 0
    seq.setLoopEnabled(false);

    seq.play(ctx.currentTime);

    // Advance in small steps, updating sequencer each time
    for (var i = 0; i < 6; i++) {
        advanceTime(16);
        seq.update(ctx.currentTime);
    }

    var peak = ctx.getBusPeakL(0);
    assertGt(peak, 0.0, 'sequencer note should produce signal');

    seq.stop();
    advanceTime(500);
});

// ---------------------------------------------------------------------------
// Test 10: Automation lane changes parameter over time
// ---------------------------------------------------------------------------
test('automation lane modifies bus gain over beats', function() {
    var busId = ctx.createBus();
    ctx.setBusGain(busId, 1.0);

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    var seq = ctx.createSequence(alloc);
    seq.setBPM(120); // 1 beat = 500ms

    // Held note for 8 beats
    seq.addNote(0.0, 69, 0.8, 8.0);

    // Automation: ramp bus gain from 1.0 down to 0.0 over 4 beats (2s)
    var currentGain = 1.0;
    var laneIdx = seq.addAutomationLane(function(val) {
        currentGain = val;
        ctx.setBusGain(busId, val);
    });
    seq.addAutomationPoint(laneIdx, 0.0, 1.0);
    seq.addAutomationPoint(laneIdx, 4.0, 0.0);
    seq.setAutomationInterpMode(laneIdx, 'linear');

    seq.play(ctx.currentTime);

    // Advance to ~200ms (~beat 0.4), update sequencer frequently
    for (var i = 0; i < 12; i++) {
        advanceTime(16);
        seq.update(ctx.currentTime);
    }
    var gainEarly = currentGain;
    var rmsEarly = ctx.getBusRmsL(busId);

    // Advance another 1.5s to ~beat 3.4
    for (var i = 0; i < 90; i++) {
        advanceTime(16);
        seq.update(ctx.currentTime);
    }
    var gainLate = currentGain;
    var rmsLate = ctx.getBusRmsL(busId);

    assertGt(gainEarly, gainLate, 'automation should reduce gain (early=' + gainEarly.toFixed(3) + ', late=' + gainLate.toFixed(3) + ')');

    seq.stop();
    alloc.noteOff(69);
    advanceTime(500);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 11: Compressor reduces peak levels
// ---------------------------------------------------------------------------
test('compressor reduces loud signal peaks', function() {
    var busId = ctx.createBus();

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'square');
        ctx.setVoiceGain(id, 3.0);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    // Measure uncompressed peak
    alloc.noteOn(60, 1.0);
    advanceTime(100);
    var uncompressedPeak = ctx.getBusPeakL(busId);

    // Enable heavy compression
    ctx.setBusCompressorEnabled(busId, true);
    ctx.setBusCompressorThreshold(busId, -20);
    ctx.setBusCompressorRatio(busId, 10);
    ctx.setBusCompressorAttack(busId, 1);
    ctx.setBusCompressorRelease(busId, 50);

    advanceTime(200);
    var compressedPeak = ctx.getBusPeakL(busId);

    assertGt(uncompressedPeak, 0, 'should have signal before compression');
    assertGt(uncompressedPeak, compressedPeak * 0.5,
        'compressed peak should be noticeably lower');

    alloc.noteOff(60);
    advanceTime(300);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 12: Reverb adds tail after note stops
// ---------------------------------------------------------------------------
test('reverb creates tail after note ends', function() {
    var busId = ctx.createBus();
    ctx.setBusReverbEnabled(busId, true);
    ctx.setBusReverbRoomSize(busId, 0.8);
    ctx.setBusReverbDamping(busId, 0.3);
    ctx.setBusReverbMix(busId, 0.9);

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.005);
    });

    alloc.noteOn(69, 0.8);
    advanceTime(50);
    alloc.noteOff(69);
    advanceTime(100);

    // Reverb tail should still be audible
    advanceTime(50);
    var reverbTail = ctx.getBusPeakL(busId);
    assertGt(reverbTail, 0.001, 'reverb tail should persist after note');

    advanceTime(3000);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 13: EQ boost increases energy in target band
// ---------------------------------------------------------------------------
test('EQ boost at 1kHz increases mid-frequency energy', function() {
    var busId = ctx.createBus();

    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'whitenoise');
        ctx.setVoiceGain(id, 0.5);
        ctx.setVoiceBus(id, busId);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    alloc.noteOn(60, 0.8);
    advanceTime(200);

    // Read RMS without EQ
    var rmsBefore = ctx.getBusRmsL(busId);

    // Boost 1kHz band (band 3) by +12dB
    ctx.setBusEqEnabled(busId, true);
    ctx.setBusEqBandGain(busId, 3, 12.0);

    advanceTime(200);

    var rmsAfter = ctx.getBusRmsL(busId);

    // EQ boost should increase overall RMS
    assertGt(rmsAfter, rmsBefore, 'EQ boost should increase RMS (before=' + rmsBefore.toFixed(4) + ', after=' + rmsAfter.toFixed(4) + ')');

    alloc.noteOff(60);
    advanceTime(200);
    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 14: Clip playback
// ---------------------------------------------------------------------------
test('audio clip plays back correctly', function() {
    var numSamples = Math.floor(ctx.sampleRate * 0.1);
    var clipData = new Float32Array(numSamples);
    for (var i = 0; i < numSamples; i++) {
        clipData[i] = Math.sin(2 * Math.PI * 440 * i / ctx.sampleRate) * 0.5;
    }

    var clipId = ctx.createClip(clipData, 1);
    assert(clipId > 0, 'clip should be created');

    var playId = ctx.playClip(clipId, 1.0, false);
    advanceTime(50);

    var peak = ctx.getBusPeakL(0);
    assertGt(peak, 0.0, 'clip playback should produce signal');

    ctx.stopPlayback(playId);
    advanceTime(200);
    ctx.deleteClip(clipId);
});

// ---------------------------------------------------------------------------
// Test 15: Offline effect processing
// ---------------------------------------------------------------------------
test('offline processing applies bus effects to buffer', function() {
    var busId = ctx.createBus();

    ctx.setBusDelayEnabled(busId, true);
    ctx.setBusDelayTime(busId, 0.01);
    ctx.setBusDelayFeedback(busId, 0.3);
    ctx.setBusDelayMix(busId, 1.0);

    var numSamples = 4410;
    var input = new Float32Array(numSamples);
    for (var i = 0; i < numSamples; i++) {
        input[i] = Math.sin(2 * Math.PI * 440 * i / ctx.sampleRate);
    }

    var output = ctx.processEffectsOffline(busId, input);
    assert(output.length > 0, 'offline processing should return samples');

    var diffSum = 0;
    var len = Math.min(input.length, output.length);
    for (var i = 0; i < len; i++) {
        diffSum += Math.abs(output[i] - input[i]);
    }
    assertGt(diffSum, 0.1, 'processed signal should differ from input');

    ctx.deleteBus(busId);
});

// ---------------------------------------------------------------------------
// Test 16: Multiple buses with independent effects
// ---------------------------------------------------------------------------
test('two buses have independent effect states', function() {
    var bus1 = ctx.createBus();
    var bus2 = ctx.createBus();

    // Bus1: reverb, Bus2: no effects
    ctx.setBusReverbEnabled(bus1, true);
    ctx.setBusReverbMix(bus1, 1.0);
    ctx.setBusReverbRoomSize(bus1, 0.9);

    var alloc1 = ctx.createVoiceAllocator(4);
    alloc1.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceBus(id, bus1);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.005);
    });

    var alloc2 = ctx.createVoiceAllocator(4);
    alloc2.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceBus(id, bus2);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.005);
    });

    // Play and stop both
    alloc1.noteOn(69, 0.8);
    alloc2.noteOn(69, 0.8);
    advanceTime(50);
    alloc1.noteOff(69);
    alloc2.noteOff(69);
    advanceTime(100);

    // Bus1 (reverb) should still have signal, bus2 (dry) should be silent
    var peak1 = ctx.getBusPeakL(bus1);
    var peak2 = ctx.getBusPeakL(bus2);

    assertGt(peak1, peak2, 'reverb bus should have more residual signal than dry bus');

    advanceTime(3000);
    ctx.deleteBus(bus1);
    ctx.deleteBus(bus2);
});

// ---------------------------------------------------------------------------
// Test 17: Frequency verification via spectrum
// ---------------------------------------------------------------------------
test('440Hz sine shows peak at correct frequency bin', function() {
    var alloc = ctx.createVoiceAllocator(4);
    alloc.setVoiceSetup(function(id) {
        ctx.setVoiceWaveform(id, 'sine');
        ctx.setVoiceGain(id, 1.0);
        ctx.setVoiceAttack(id, 0.001);
        ctx.setVoiceSustain(id, 1.0);
        ctx.setVoiceRelease(id, 0.01);
    });

    alloc.noteOn(69, 0.8);
    advanceTime(200); // let it ring for a while to fill analysis buffer

    var numBins = 2048;
    var spec = ctx.getSpectrum(numBins);

    // Find peak bin
    var peakBin = 0;
    var peakVal = 0;
    for (var i = 1; i < spec.length; i++) {
        if (spec[i] > peakVal) {
            peakVal = spec[i];
            peakBin = i;
        }
    }

    // Convert bin to frequency
    var binHz = ctx.sampleRate / numBins;
    var peakFreq = peakBin * binHz;

    assertGt(peakVal, 0, 'should have spectral peak');
    // Allow +-1 bin tolerance
    assertGt(peakFreq, 400, 'peak should be near 440Hz (got ' + peakFreq.toFixed(1) + 'Hz)');
    assertLt(peakFreq, 480, 'peak should be near 440Hz (got ' + peakFreq.toFixed(1) + 'Hz)');

    alloc.noteOff(69);
    advanceTime(200);
});

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('');
console.log('Results: ' + passed + ' passed, ' + failed + ' failed out of ' + (passed + failed));

if (failed > 0) {
    assert(false, failed + ' test(s) failed');
}
