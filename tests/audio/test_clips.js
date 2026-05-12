// Audio clip creation, playback, getClipWaveform binning.

const ctx = new AudioContext();
const sr = ctx.sampleRate;

// Build a 0.5s sine tone clip at 440Hz, mono
const N = Math.floor(sr * 0.5);
const data = new Float32Array(N);
for (let i = 0; i < N; i++) {
    data[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
}

const clipId = ctx.createClip(data, 1);
console.log('clipId:', clipId);
assert(typeof clipId === 'number' && clipId >= 0, 'createClip returns valid id, got ' + clipId);

const n = ctx.getClipSampleCount(clipId);
console.log('sample count:', n);
assert(n === N, 'getClipSampleCount matches input, expected ' + N + ' got ' + n);

const ch = ctx.getClipChannels(clipId);
console.log('channels:', ch);
assert(ch === 1, 'getClipChannels is 1 for mono, got ' + ch);

// Waveform binning: numBins=10 -> length 20 (min,max pairs)
const bins = 10;
const wf = ctx.getClipWaveform(clipId, bins);
console.log('wf type:', wf && wf.constructor.name, 'len:', wf && wf.length);
assert(wf && wf.length === bins * 2, 'getClipWaveform returns length ' + (bins*2) + ', got ' + (wf && wf.length));

// For a 440Hz sine at 0.5 amplitude, each min should be near -0.5 and each max near +0.5
let minOk = true, maxOk = true;
for (let i = 0; i < bins; i++) {
    const mn = wf[i*2], mx = wf[i*2 + 1];
    if (mn > -0.3 || mn < -0.6) { minOk = false; console.log('bin', i, 'min out of range:', mn); }
    if (mx <  0.3 || mx >  0.6) { maxOk = false; console.log('bin', i, 'max out of range:', mx); }
}
assert(minOk, 'all waveform bin mins near -0.5');
assert(maxOk, 'all waveform bin maxes near +0.5');

// playClip returns a playback id
const pbId = ctx.playClip(clipId, 1.0, false);
console.log('playback id:', pbId);
assert(typeof pbId === 'number' && pbId >= 0, 'playClip returns playback id, got ' + pbId);
sleep(50);
const pos = ctx.getPlaybackPosition(pbId);
console.log('position:', pos);
assert(typeof pos === 'number' && pos >= 0, 'getPlaybackPosition returns non-negative number, got ' + pos);

ctx.setPlaybackRate(pbId, 0.5);
ctx.setPlaybackPan(pbId, -0.5);
ctx.setPlaybackGain(pbId, 0.7);
ctx.setPlaybackLoop(pbId, true);
ctx.stopPlayback(pbId);

ctx.deleteClip(clipId);
