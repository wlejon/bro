// ADSR envelope shape — measured via bus peak meter sampled at each stage.

const ctx = new AudioContext();
const osc = ctx.createOscillator();
osc.type = 'sine';
osc.frequency.value = 440;
osc.attack.value  = 0.05;   // 50ms
osc.decay.value   = 0.05;   // 50ms
osc.sustain.value = 0.5;
osc.release.value = 0.1;    // 100ms
osc.connect(ctx.destination);

osc.start();
sleep(20);
const attackPeak  = ctx.getBusPeakL(0);   // attack ramping up
sleep(40);
const peakAfterAttack = ctx.getBusPeakL(0); // attack done, near max
sleep(80);
const sustainPeak = ctx.getBusPeakL(0);   // in sustain
osc.stop();
sleep(60);
const releaseMid  = ctx.getBusPeakL(0);   // release ramping down
sleep(120);
const tailPeak    = ctx.getBusPeakL(0);   // well past release

console.log('attack(20ms):', attackPeak);
console.log('post-attack(60ms):', peakAfterAttack);
console.log('sustain(140ms):', sustainPeak);
console.log('release-mid(stop+60ms):', releaseMid);
console.log('tail(stop+180ms):', tailPeak);

// Attack peak is mid-ramp, post-attack peak is at top before decay
assert(peakAfterAttack > attackPeak, 'post-attack peak > mid-attack peak (' + peakAfterAttack + ' > ' + attackPeak + ')');
// Sustain at 0.5 is below the post-attack peak (envelope started at 1.0)
assert(sustainPeak < peakAfterAttack * 0.95, 'sustain (0.5) is below post-attack peak (1.0)');
assert(sustainPeak > 0.005, 'sustain has meaningful amplitude (' + sustainPeak + ')');
// Release should bring level down
assert(releaseMid < sustainPeak, 'release mid is quieter than sustain (' + releaseMid + ' < ' + sustainPeak + ')');
assert(tailPeak < sustainPeak * 0.2, 'tail is much quieter than sustain (' + tailPeak + ' < 0.2*' + sustainPeak + ')');
