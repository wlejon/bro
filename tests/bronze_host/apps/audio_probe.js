// The audio probe: Web Audio integration in bronze_host.
//
// Tests:
// - AudioContext & webkitAudioContext creation, currentTime, sampleRate, state, destination, listener
// - createGain(), gain.value, gain.setValueAtTime(), gain.connect(), gain.disconnect()
// - createOscillator(), osc.type, osc.frequency.value, osc.detune.value, osc.connect(), osc.start(), osc.stop()
// - createBiquadFilter(), filter.type, filter.frequency.value, filter.Q.value, filter.gain.value, filter.connect()
// - createAnalyser(), analyser.fftSize, analyser.frequencyBinCount, getFloatFrequencyData, getByteFrequencyData, getFloatTimeDomainData, getByteTimeDomainData
// - createBuffer(), buffer.numberOfChannels, buffer.length, buffer.sampleRate, buffer.duration, getChannelData(0)
// - createBufferSource(), src.buffer, src.loop, src.playbackRate, src.connect(), src.start(), src.stop()
// - Bus & spatial listener APIs (createBus, setBusGain, setBusPan, setBusDelayEnabled, etc.)
// - Clip API (createClip, playClip, stopPlayback, deleteClip)
// - decodeAudioData() with procedural WAV bytes

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

const got = {};
function record(name, value) {
    got[name] = value;
}

// ---------------------------------------------------------------------------
// 1. AudioContext creation & properties
// ---------------------------------------------------------------------------

say('global.hasAudioContext', typeof AudioContext === 'function');
say('global.hasWebkitAudioContext', typeof webkitAudioContext === 'function');
say('global.hasAudioNode', typeof AudioNode === 'function');
say('global.hasAudioParam', typeof AudioParam === 'function');
say('global.hasGainNode', typeof GainNode === 'function');
say('global.hasOscillatorNode', typeof OscillatorNode === 'function');
say('global.hasAudioBuffer', typeof AudioBuffer === 'function');
say('global.hasAudioBufferSourceNode', typeof AudioBufferSourceNode === 'function');
say('global.hasBiquadFilterNode', typeof BiquadFilterNode === 'function');
say('global.hasAnalyserNode', typeof AnalyserNode === 'function');
say('global.hasPannerNode', typeof PannerNode === 'function');
say('global.hasStereoPannerNode', typeof StereoPannerNode === 'function');
say('global.hasDelayNode', typeof DelayNode === 'function');
say('global.hasDynamicsCompressorNode', typeof DynamicsCompressorNode === 'function');
say('global.hasWaveShaperNode', typeof WaveShaperNode === 'function');
say('global.hasConvolverNode', typeof ConvolverNode === 'function');
say('global.hasChannelSplitterNode', typeof ChannelSplitterNode === 'function');
say('global.hasChannelMergerNode', typeof ChannelMergerNode === 'function');
say('global.hasPeriodicWave', typeof PeriodicWave === 'function');

const ctx = new AudioContext();
say('ctx.state', ctx.state);
say('ctx.sampleRateValid', typeof ctx.sampleRate === 'number' && ctx.sampleRate > 0);
say('ctx.currentTimeValid', typeof ctx.currentTime === 'number' && ctx.currentTime >= 0);
say('ctx.hasDestination', typeof ctx.destination === 'object' && ctx.destination !== null);
say('ctx.hasListener', typeof ctx.listener === 'object' && ctx.listener !== null);

ctx.listener.setPosition(1, 2, 3);
ctx.listener.setOrientation(0, 0, -1, 0, 1, 0);
say('listener.methods', typeof ctx.listener.setPosition === 'function' && typeof ctx.listener.setOrientation === 'function');

// ---------------------------------------------------------------------------
// 2. GainNode & AudioParam
// ---------------------------------------------------------------------------

const gain = ctx.createGain();
say('gain.defaultVal', gain.gain.value);
gain.gain.value = 0.5;
say('gain.setVal', gain.gain.value);
const chain = gain.gain.setValueAtTime(0.8, 1.0);
say('gain.chain', chain === gain.gain);
say('gain.afterAutomation', Math.round(gain.gain.value * 100) / 100);
const conn = gain.connect(ctx.destination);
say('gain.connect', conn === ctx.destination);
gain.disconnect();
say('gain.disconnect', true);

// ---------------------------------------------------------------------------
// 3. OscillatorNode
// ---------------------------------------------------------------------------

const osc = ctx.createOscillator();
say('osc.type', osc.type);
osc.type = 'sawtooth';
say('osc.newType', osc.type);
say('osc.freqDefault', osc.frequency.value);
osc.frequency.value = 880;
say('osc.freqSet', osc.frequency.value);
say('osc.detuneDefault', osc.detune.value);
const oscConn = osc.connect(gain);
say('osc.connect', oscConn === gain);
osc.start(0);
osc.stop(0);
say('osc.lifecycle', true);

// ---------------------------------------------------------------------------
// 4. BiquadFilterNode
// ---------------------------------------------------------------------------

const filter = ctx.createBiquadFilter();
say('filter.defaultType', filter.type);
filter.type = 'bandpass';
say('filter.newType', filter.type);
say('filter.freqDefault', filter.frequency.value);
filter.frequency.value = 1000;
say('filter.freqSet', filter.frequency.value);
say('filter.qDefault', filter.Q.value);
say('filter.gainDefault', filter.gain.value);
const fConn = filter.connect(ctx.destination);
say('filter.connect', fConn === ctx.destination);

// ---------------------------------------------------------------------------
// 5. AnalyserNode
// ---------------------------------------------------------------------------

const analyser = ctx.createAnalyser();
say('analyser.fftSize', analyser.fftSize);
say('analyser.binCount', analyser.frequencyBinCount);
analyser.fftSize = 1024;
say('analyser.newFftSize', analyser.fftSize);
say('analyser.newBinCount', analyser.frequencyBinCount);

const floatData = new Float32Array(analyser.frequencyBinCount);
analyser.getFloatFrequencyData(floatData);
say('analyser.floatDataLen', floatData.length);

const byteData = new Uint8Array(analyser.frequencyBinCount);
analyser.getByteFrequencyData(byteData);
say('analyser.byteDataLen', byteData.length);

const timeFloat = new Float32Array(analyser.fftSize);
analyser.getFloatTimeDomainData(timeFloat);
say('analyser.timeFloatLen', timeFloat.length);

const timeByte = new Uint8Array(analyser.fftSize);
analyser.getByteTimeDomainData(timeByte);
say('analyser.timeByteLen', timeByte.length);

// ---------------------------------------------------------------------------
// 6. AudioBuffer
// ---------------------------------------------------------------------------

const buffer = ctx.createBuffer(2, 44100, 44100);
say('buf.channels', buffer.numberOfChannels);
say('buf.length', buffer.length);
say('buf.sampleRate', buffer.sampleRate);
say('buf.duration', buffer.duration);
const ch0 = buffer.getChannelData(0);
say('buf.ch0Len', ch0.length);
ch0[0] = 0.5;
say('buf.ch0Write', buffer.getChannelData(0)[0]);

// ---------------------------------------------------------------------------
// 7. AudioBufferSourceNode
// ---------------------------------------------------------------------------

const src = ctx.createBufferSource();
src.buffer = buffer;
say('src.hasBuffer', src.buffer === buffer);
say('src.loopDefault', src.loop);
src.loop = true;
say('src.loopSet', src.loop);
say('src.playbackRate', src.playbackRate.value);
const sConn = src.connect(ctx.destination);
say('src.connect', sConn === ctx.destination);
src.start(0);
src.stop(0);
say('src.lifecycle', true);

// ---------------------------------------------------------------------------
// 8. PannerNode & StereoPannerNode
// ---------------------------------------------------------------------------

const panner = ctx.createPanner();
say('panner.panningModel', panner.panningModel);
say('panner.distanceModel', panner.distanceModel);
say('panner.posDefault', panner.positionX.value);
panner.positionX.value = 5.0;
say('panner.posSet', panner.positionX.value);
panner.connect(ctx.destination);
say('panner.connect', true);

const stereoPanner = ctx.createStereoPanner();
say('stereoPanner.panDefault', stereoPanner.pan.value);
stereoPanner.pan.value = 0.5;
say('stereoPanner.panSet', stereoPanner.pan.value);
stereoPanner.connect(ctx.destination);
say('stereoPanner.connect', true);

// ---------------------------------------------------------------------------
// 9. DelayNode
// ---------------------------------------------------------------------------

const delay = ctx.createDelay(2.0);
say('delay.default', delay.delayTime.value);
delay.delayTime.value = 0.25;
say('delay.set', delay.delayTime.value);
delay.connect(ctx.destination);
say('delay.connect', true);

// ---------------------------------------------------------------------------
// 10. DynamicsCompressorNode
// ---------------------------------------------------------------------------

const compressor = ctx.createDynamicsCompressor();
say('compressor.thresholdDefault', compressor.threshold.value);
compressor.threshold.value = -20;
say('compressor.thresholdSet', compressor.threshold.value);
say('compressor.kneeDefault', compressor.knee.value);
say('compressor.ratioDefault', compressor.ratio.value);
compressor.connect(ctx.destination);
say('compressor.connect', true);

// ---------------------------------------------------------------------------
// 11. WaveShaperNode
// ---------------------------------------------------------------------------

const shaper = ctx.createWaveShaper();
say('shaper.oversampleDefault', shaper.oversample);
shaper.oversample = '4x';
say('shaper.oversampleSet', shaper.oversample);
const curve = new Float32Array([-1, 0, 1]);
shaper.curve = curve;
say('shaper.hasCurve', shaper.curve !== null);
shaper.connect(ctx.destination);
say('shaper.connect', true);

// ---------------------------------------------------------------------------
// 12. ConvolverNode
// ---------------------------------------------------------------------------

const convolver = ctx.createConvolver();
say('convolver.normalizeDefault', convolver.normalize);
convolver.normalize = false;
say('convolver.normalizeSet', convolver.normalize);
convolver.buffer = buffer;
say('convolver.hasBuffer', convolver.buffer === buffer);
convolver.connect(ctx.destination);
say('convolver.connect', true);

// ---------------------------------------------------------------------------
// 13. ChannelSplitterNode & ChannelMergerNode
// ---------------------------------------------------------------------------

const splitter = ctx.createChannelSplitter(6);
say('splitter.outputs', splitter.numberOfOutputs);
splitter.connect(ctx.destination);
say('splitter.connect', true);

const merger = ctx.createChannelMerger(6);
say('merger.inputs', merger.numberOfInputs);
merger.connect(ctx.destination);
say('merger.connect', true);

// ---------------------------------------------------------------------------
// 14. PeriodicWave
// ---------------------------------------------------------------------------

const real = new Float32Array([0, 1, 0.5]);
const imag = new Float32Array([0, 0, 0]);
const pWave = ctx.createPeriodicWave(real, imag);
say('wave.created', pWave instanceof PeriodicWave);
osc.setPeriodicWave(pWave);
say('osc.setPeriodicWave', true);

// ---------------------------------------------------------------------------
// 15. Bus API & Effects
// ---------------------------------------------------------------------------

const busId = ctx.createBus();
say('bus.created', typeof busId === 'number' && busId >= 0);
ctx.setBusGain(busId, 0.75);
ctx.setBusPan(busId, -0.5);
ctx.setBusDelayEnabled(busId, true);
ctx.setBusDelayTime(busId, 0.25);
ctx.setBusReverbEnabled(busId, true);
ctx.setBusChorusEnabled(busId, true);
ctx.setBusCompressorEnabled(busId, true);
ctx.setDelayEnabled(true);
ctx.setReverbEnabled(true);
ctx.deleteBus(busId);
say('bus.configured', true);

// ---------------------------------------------------------------------------
// 9. Clip API
// ---------------------------------------------------------------------------

const clipBuf = ctx.createBuffer(1, 1000, 44100);
const clipId = ctx.createClip(clipBuf);
say('clip.created', typeof clipId === 'number' && clipId >= 0);
const playId = ctx.playClip(clipId, 0.8, false);
say('clip.played', typeof playId === 'number' && playId >= 0);
ctx.stopPlayback(playId);
ctx.deleteClip(clipId);
say('clip.deleted', true);

// ---------------------------------------------------------------------------
// 10. decodeAudioData() with procedural WAV buffer
// ---------------------------------------------------------------------------

function createSineWavBytes(numSamples, sampleRate, freq) {
    const headerSize = 44;
    const dataSize = numSamples * 2;
    const totalSize = headerSize + dataSize;
    const buf = new ArrayBuffer(totalSize);
    const view = new DataView(buf);

    // "RIFF"
    view.setUint8(0, 0x52); view.setUint8(1, 0x49); view.setUint8(2, 0x46); view.setUint8(3, 0x46);
    view.setUint32(4, totalSize - 8, true);
    // "WAVE"
    view.setUint8(8, 0x57); view.setUint8(9, 0x41); view.setUint8(10, 0x56); view.setUint8(11, 0x45);
    // "fmt "
    view.setUint8(12, 0x66); view.setUint8(13, 0x6D); view.setUint8(14, 0x74); view.setUint8(15, 0x20);
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true); // PCM
    view.setUint16(22, 1, true); // Mono
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 2, true); // ByteRate
    view.setUint16(32, 2, true); // BlockAlign
    view.setUint16(34, 16, true); // BitsPerSample
    // "data"
    view.setUint8(36, 0x64); view.setUint8(37, 0x61); view.setUint8(38, 0x74); view.setUint8(39, 0x61);
    view.setUint32(40, dataSize, true);

    for (let i = 0; i < numSamples; i++) {
        const t = i / sampleRate;
        const s = Math.sin(2 * Math.PI * freq * t);
        const sample16 = Math.max(-32768, Math.min(32767, Math.floor(s * 32767)));
        view.setInt16(44 + i * 2, sample16, true);
    }
    return buf;
}

const wavBytes = createSineWavBytes(4410, 44100, 440);
ctx.decodeAudioData(wavBytes).then((decoded) => {
    record('decode.channels', decoded.numberOfChannels);
    record('decode.length', decoded.length);
    record('decode.sampleRate', decoded.sampleRate);
    record('decode.hasData', decoded.getChannelData(0).length);
}).catch((err) => {
    record('decode.error', err && err.message ? err.message : String(err));
});

// ---------------------------------------------------------------------------
// Report async results & finish
// ---------------------------------------------------------------------------

const REPORT = [
    'decode.channels',
    'decode.length',
    'decode.sampleRate',
    'decode.hasData',
];

let frames = 0;
function tick() {
    if (++frames < 4) {
        requestAnimationFrame(tick);
        return;
    }
    for (const name of REPORT) {
        say(name, Object.prototype.hasOwnProperty.call(got, name) ? got[name] : 'absent');
    }
    say('done', 1);
}
requestAnimationFrame(tick);
