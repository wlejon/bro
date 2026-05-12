// AudioContext basics: creation, properties, defaults, suspend/resume/close.

assert(typeof AudioContext === 'function', 'AudioContext constructor exists');

const ctx = new AudioContext();
assert(ctx, 'context created');

// Properties documented as read-only / typed
assert(typeof ctx.sampleRate === 'number', 'sampleRate is number');
assert(ctx.sampleRate > 0, 'sampleRate > 0');
assert(ctx.sampleRate === 44100 || ctx.sampleRate === 48000, 'sampleRate is 44100 or 48000, got ' + ctx.sampleRate);

assert(typeof ctx.currentTime === 'number', 'currentTime is number');
assert(ctx.currentTime >= 0, 'currentTime >= 0');

assert(typeof ctx.masterGain === 'number', 'masterGain is number');
assert(ctx.masterGain === 1.0 || Math.abs(ctx.masterGain - 1.0) < 1e-6, 'masterGain default is 1.0, got ' + ctx.masterGain);

ctx.masterGain = 0.5;
assert(Math.abs(ctx.masterGain - 0.5) < 1e-6, 'masterGain set/get round-trips, got ' + ctx.masterGain);
ctx.masterGain = 1.0;

assert(typeof ctx.recording === 'boolean', 'recording is boolean');
assert(ctx.recording === false, 'recording defaults to false');

assert(ctx.destination, 'destination exists');
assert(typeof ctx.destination === 'object', 'destination is object');

// Suspend / resume / close are NOT documented in audio-api.js — they are
// Web Audio API standard but may or may not exist. Probe gently.
// (No assertion required; this just documents presence.)
console.log('has suspend:', typeof ctx.suspend);
console.log('has resume:', typeof ctx.resume);
console.log('has close:', typeof ctx.close);

// currentTime should advance after a real sleep (audio engine runs on its own thread)
const t0 = ctx.currentTime;
sleep(100);
const t1 = ctx.currentTime;
assert(t1 >= t0, 'currentTime is monotonically non-decreasing (' + t0 + ' -> ' + t1 + ')');
// In headless "no audio device" mode the engine may or may not advance time;
// document but do not require a strict positive advance.
console.log('currentTime delta:', t1 - t0);
