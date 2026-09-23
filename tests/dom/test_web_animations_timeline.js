// What el.animate() returns beyond playback control: effect (a
// KeyframeEffect over the record), timeline (document.timeline), startTime,
// and the ready promise; and document.timeline.currentTime itself.

const div = document.createElement('div');
document.body.appendChild(div);
flush();

// ---- document.timeline -----------------------------------------------------
const tl = document.timeline;
assert(tl && typeof tl === 'object', 'document.timeline exists');
assert(tl === document.timeline, 'document.timeline is one object');
assert(typeof DocumentTimeline === 'function' && tl instanceof DocumentTimeline,
       'it is a DocumentTimeline');
assert(tl instanceof AnimationTimeline, 'which is an AnimationTimeline');
const t0 = tl.currentTime;
assert(typeof t0 === 'number', 'currentTime is a number');
advanceTime(100);
const t1 = tl.currentTime;
assert(t1 - t0 >= 99 && t1 - t0 <= 101, 'currentTime advances with the clock, delta ' + (t1 - t0));

// ---- timeline / startTime --------------------------------------------------
const anim = div.animate([{ opacity: 0 }, { opacity: 1, offset: 1 }],
                         { duration: 1000, delay: 100, endDelay: 50, iterations: 2,
                           direction: 'alternate', fill: 'both', easing: 'ease-in' });
assert(anim.timeline === document.timeline, 'animation.timeline is document.timeline');
assert(typeof anim.startTime === 'number', 'a running animation has a startTime');
advanceTime(250);
{
    const expect = (document.timeline.currentTime - anim.startTime) * anim.playbackRate;
    assert(Math.abs(anim.currentTime - expect) < 1e-6,
           'currentTime = (timeline - startTime) * rate: ' + anim.currentTime + ' vs ' + expect);
}
anim.pause();
assert(anim.startTime === null, 'a paused animation has no startTime');
anim.play();
assert(typeof anim.startTime === 'number', 'playing again resolves it');

// Setting startTime seeks.
anim.startTime = document.timeline.currentTime - 300;
assert(Math.abs(anim.currentTime - 300) < 1e-6, 'startTime setter seeks, got ' + anim.currentTime);

// ---- ready -----------------------------------------------------------------
const ready = anim.ready;
assert(ready instanceof Promise, 'ready is a Promise');
assert(anim.ready === ready, 'ready is stable');
const readyValue = await ready;
assert(readyValue === anim, 'ready resolves with the animation');

// ---- effect ----------------------------------------------------------------
const effect = anim.effect;
assert(effect && typeof effect === 'object', 'effect exists');
assert(anim.effect === effect, 'effect is stable');
assert(typeof KeyframeEffect === 'function' && effect instanceof KeyframeEffect,
       'effect is a KeyframeEffect');
assert(effect instanceof AnimationEffect, 'which is an AnimationEffect');
assert(effect.target === div, 'effect.target is the animated element');

const timing = effect.getTiming();
assert(timing.duration === 1000, 'timing.duration');
assert(timing.delay === 100 && timing.endDelay === 50, 'timing delay / endDelay');
assert(timing.iterations === 2, 'timing.iterations');
assert(timing.direction === 'alternate', 'timing.direction');
assert(timing.fill === 'both', 'timing.fill');
assert(timing.easing === 'ease-in', 'timing.easing, got ' + timing.easing);

// Seek to a known point: local 600 -> active 500 -> iteration 0, halfway.
anim.currentTime = 600;
let ct = effect.getComputedTiming();
assert(ct.activeDuration === 2000, 'activeDuration = duration * iterations');
assert(ct.endTime === 2150, 'endTime = delay + active + endDelay, got ' + ct.endTime);
assert(ct.localTime === 600, 'localTime is the animation currentTime');
assert(ct.currentIteration === 0, 'first iteration');
assert(ct.progress > 0 && ct.progress < 0.5, 'ease-in progress below linear at 0.5, got ' + ct.progress);

// Second iteration of an alternate animation runs backwards.
anim.currentTime = 100 + 1000 + 250;
ct = effect.getComputedTiming();
assert(ct.currentIteration === 1, 'second iteration, got ' + ct.currentIteration);
const linearEquivalentDirected = 0.75;   // 1 - 0.25
assert(ct.progress < linearEquivalentDirected, 'alternate reverses, eased: ' + ct.progress);

// Before phase with fill backwards: iteration 0, progress 0.
anim.currentTime = 50;
ct = effect.getComputedTiming();
assert(ct.currentIteration === 0 && ct.progress === 0, 'backwards fill holds the start');

// After phase with fill forwards: last iteration, alternate ends at 0.
anim.currentTime = 5000;
ct = effect.getComputedTiming();
assert(ct.currentIteration === 1, 'after phase reports the last iteration');
assert(ct.progress === 0, 'alternate x2 ends at progress 0, got ' + ct.progress);

// No fill: outside the active interval there is no progress.
const plain = div.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 100, delay: 100 });
const pct = plain.effect.getComputedTiming();
assert(pct.progress === null && pct.currentIteration === null,
       'before phase without fill has null progress');
assert(plain.effect !== effect, 'each animation has its own effect');

// Keyframes come back with offsets and camelCase properties.
const moved = div.animate({ backgroundColor: ['red', 'blue'], opacity: [0, 1] }, 500);
const kfs = moved.effect.getKeyframes();
assert(Array.isArray(kfs) && kfs.length === 2, 'two keyframes, got ' + kfs.length);
assert(kfs[0].offset === 0 && kfs[1].offset === 1, 'offsets 0 and 1');
assert(kfs[0].computedOffset === 0, 'computedOffset');
assert(kfs[0].easing === 'linear', 'default keyframe easing is linear');
assert(kfs[0].backgroundColor === 'red' && kfs[1].backgroundColor === 'blue',
       'camelCase property names');
assert(kfs[1].opacity === '1' && kfs[0].opacity === '0',
       'a number value reads back as its JS string, got ' + JSON.stringify(kfs[1].opacity));
const half = div.animate([{ opacity: 0.5 }, { opacity: 1 }], 500);
assert(half.effect.getKeyframes()[0].opacity === '0.5', 'fractional number value');

// A cancelled animation has no startTime and no local time.
moved.cancel();
assert(moved.startTime === null, 'cancelled: startTime null');
assert(moved.effect.getComputedTiming().localTime === null, 'cancelled: localTime null');

console.log('web animations timeline/effect: OK');
