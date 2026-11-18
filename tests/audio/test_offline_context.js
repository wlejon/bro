// OfflineAudioContext renders its graph into an AudioBuffer (broaudio's
// host_audio_offline*.cpp; sample-level checks against Chromium are
// broaudio's test_audio_api_offline). Here: it is installed in an app realm,
// renders oscillator -> gain -> destination with Chromium's first samples
// and RMS, resolves startRendering and then fires `complete`, and a node
// made by a live AudioContext beside it still plays live. Tone.Offline
// stopped at "Missing the native OfflineAudioContext constructor".

assert(typeof OfflineAudioContext === 'function', 'OfflineAudioContext exists');
assert(typeof OfflineAudioCompletionEvent === 'function', 'OfflineAudioCompletionEvent exists');

const ctx = new OfflineAudioContext(1, 4410, 44100);
const osc = ctx.createOscillator();
const gain = ctx.createGain();
gain.gain.value = 0.5;
osc.connect(gain).connect(ctx.destination);
osc.start();
assert(osc.context === ctx, 'a node knows its offline context');

let rendered = null, completed = null;
const order = [];
ctx.oncomplete = (e) => { completed = e.renderedBuffer; order.push('complete'); };
ctx.startRendering().then((b) => { rendered = b; order.push('resolved'); });
advanceTime(20);
assert(rendered && completed === rendered, 'the promise and the complete event carry one buffer');
assert(order.join() === 'resolved,complete', 'the promise resolves before complete fires, got ' + order.join());
const d = rendered.getChannelData(0);
const near = (a, b) => Math.abs(a - b) < 1e-5;
assert(near(d[0], 0) && near(d[1], 0.031324) && near(d[2], 0.062525) && near(d[1000], -0.070998),
       'first samples are Chromium\'s, got ' + [d[0], d[1], d[2], d[1000]].join());
let s = 0;
for (let i = 0; i < d.length; i++) s += d[i] * d[i];
assert(near(Math.sqrt(s / d.length), 0.353553), 'RMS is Chromium\'s');
assert(ctx.state === 'closed', 'closed after rendering');

// A live context beside it is untouched.
const live = new AudioContext();
const o2 = live.createOscillator();
o2.connect(live.destination);
o2.start();
o2.stop();
assert(live.state !== 'closed', 'the live context still runs');
