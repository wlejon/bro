// Test element.animate() — Web Animations API (src/engine/web_animations.cpp
// + src/js/web_animation_bindings.cpp). Exercises keyframe parsing (array and
// object forms), interpolation over virtual time (opacity/length/color/
// transform), fill modes, iterations + alternate direction, pause/resume (both
// animation.pause() and bro.time), currentTime seek, playbackRate, reverse(),
// the finished promise (resolve on finish, AbortError reject on cancel),
// onfinish/oncancel, getAnimations(), and element-removal safety.

const root = document.getElementById('root');
root.innerHTML = '';

function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= eps, msg + ' (expected ~' + b + ', got ' + a + ')');
}

// ---------------------------------------------------------------------------
// 1. Basic animation: opacity + width + color + transform, linear, forwards
// ---------------------------------------------------------------------------
const box = document.createElement('div');
box.style.cssText = 'width:100px;height:50px;background-color:rgb(255,0,0);opacity:1;';
root.appendChild(box);
flush();

const anim = box.animate([
    { opacity: 0, width: '100px', backgroundColor: 'rgb(255, 0, 0)', transform: 'translateX(0px)' },
    { opacity: 1, width: '200px', backgroundColor: 'rgb(0, 0, 255)', transform: 'translateX(100px)' }
], { duration: 1000, easing: 'linear', fill: 'forwards' });

assert(anim !== null && typeof anim === 'object', 'animate() returns an Animation');
assert(anim.playState === 'running', 'new animation is running, got ' + anim.playState);
assert(typeof anim.currentTime === 'number', 'currentTime is a number');
flush();

// t = 0
let cs = getComputedStyle(box);
near(parseFloat(cs.opacity), 0, 0.02, 'opacity at 0%');
near(parseFloat(cs.width), 100, 1, 'width at 0%');

// t = 500 (50%, linear)
advanceTime(500);
cs = getComputedStyle(box);
near(parseFloat(cs.opacity), 0.5, 0.05, 'opacity at 50%');
near(parseFloat(cs.width), 150, 3, 'width at 50%');
// color midpoint: rgb(128, 0, 128) ± rounding
{
    const m = cs.backgroundColor.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)/);
    assert(m, 'bg is rgb at 50%: ' + cs.backgroundColor);
    near(parseInt(m[1]), 128, 8, 'bg red at 50%');
    near(parseInt(m[3]), 128, 8, 'bg blue at 50%');
}
{
    const tm = cs.transform.match(/translateX\(([-\d.]+)/);
    assert(tm, 'transform is translateX at 50%: ' + cs.transform);
    near(parseFloat(tm[1]), 50, 3, 'translateX at 50%');
}
near(anim.currentTime, 500, 20, 'currentTime ~500');

// t past end: fill forwards holds the final frame
advanceTime(600);
cs = getComputedStyle(box);
near(parseFloat(cs.opacity), 1, 0.02, 'opacity held at 100% (fill: forwards)');
near(parseFloat(cs.width), 200, 1, 'width held at 100% (fill: forwards)');
assert(anim.playState === 'finished', 'finished after duration, got ' + anim.playState);

// ---------------------------------------------------------------------------
// 2. fill: none snaps back to base after finishing
// ---------------------------------------------------------------------------
const b2 = document.createElement('div');
b2.style.cssText = 'width:60px;height:20px;opacity:0.9;';
root.appendChild(b2);
flush();
const a2 = b2.animate([{ opacity: 0 }, { opacity: 0.4 }], { duration: 300, easing: 'linear' });
advanceTime(150);
cs = getComputedStyle(b2);
near(parseFloat(cs.opacity), 0.2, 0.05, 'fill:none mid-flight');
advanceTime(300);
flush();
cs = getComputedStyle(b2);
near(parseFloat(cs.opacity), 0.9, 0.02, 'fill:none reverted to base after finish');
assert(a2.playState === 'finished', 'a2 finished');

// ---------------------------------------------------------------------------
// 3. iterations + alternate direction
// ---------------------------------------------------------------------------
const b3 = document.createElement('div');
b3.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b3);
flush();
const a3 = b3.animate([{ opacity: 0 }, { opacity: 1 }],
                      { duration: 200, iterations: 2, direction: 'alternate',
                        easing: 'linear', fill: 'forwards' });
advanceTime(150); // iteration 0, progress .75
cs = getComputedStyle(b3);
near(parseFloat(cs.opacity), 0.75, 0.06, 'alternate iter 0 @ .75');
advanceTime(100); // t=250 → iteration 1, progress .25, reversed → .75
cs = getComputedStyle(b3);
near(parseFloat(cs.opacity), 0.75, 0.06, 'alternate iter 1 @ .25 (reversed)');
advanceTime(100); // t=350 → iteration 1, progress .75, reversed → .25
cs = getComputedStyle(b3);
near(parseFloat(cs.opacity), 0.25, 0.06, 'alternate iter 1 @ .75 (reversed)');
advanceTime(100); // t=450 → done; alternate 2nd iteration ends at 0
cs = getComputedStyle(b3);
near(parseFloat(cs.opacity), 0, 0.03, 'alternate x2 fill-forwards ends at 0');

// ---------------------------------------------------------------------------
// 4. animation.pause() / play() and bro.time pause
// ---------------------------------------------------------------------------
const b4 = document.createElement('div');
b4.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b4);
flush();
const a4 = b4.animate([{ opacity: 0 }, { opacity: 1 }],
                      { duration: 1000, easing: 'linear', fill: 'both' });
advanceTime(400);
a4.pause();
assert(a4.playState === 'paused', 'paused state');
const pausedCt = a4.currentTime;
near(pausedCt, 400, 20, 'currentTime at pause');
advanceTime(500);
near(a4.currentTime, pausedCt, 1, 'currentTime frozen while paused');
cs = getComputedStyle(b4);
near(parseFloat(cs.opacity), 0.4, 0.05, 'value frozen while paused');
a4.play();
assert(a4.playState === 'running', 'running after resume');
advanceTime(200);
near(a4.currentTime, pausedCt + 200, 25, 'clock resumed from pause point');

// bro.time pause freezes a running animation
const beforeBroPause = a4.currentTime;
bro.time.paused = true;
advanceTime(300);
near(a4.currentTime, beforeBroPause, 1, 'bro.time.paused freezes animation clock');
bro.time.paused = false;
advanceTime(1200); // let it finish
assert(a4.playState === 'finished', 'a4 finished after resume');

// ---------------------------------------------------------------------------
// 5. currentTime seek + playbackRate + reverse
// ---------------------------------------------------------------------------
const b5 = document.createElement('div');
b5.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b5);
flush();
const a5 = b5.animate([{ opacity: 0 }, { opacity: 1 }],
                      { duration: 1000, easing: 'linear', fill: 'both' });
a5.pause();
a5.currentTime = 500;
flush();
cs = getComputedStyle(b5);
near(parseFloat(cs.opacity), 0.5, 0.03, 'seek to 50% while paused');
near(a5.currentTime, 500, 1, 'currentTime getter after seek');

a5.play();
a5.playbackRate = 2;
near(a5.playbackRate, 2, 0.001, 'playbackRate getter');
const ctBefore = a5.currentTime;
advanceTime(100);
near(a5.currentTime, ctBefore + 200, 25, 'playbackRate 2 doubles progress');

a5.reverse();
assert(a5.playbackRate === -2, 'reverse flips rate, got ' + a5.playbackRate);
const ctRev = a5.currentTime;
advanceTime(100);
assert(a5.currentTime < ctRev, 'currentTime decreasing after reverse');
advanceTime(2000); // run back to 0 → finishes
assert(a5.playState === 'finished', 'reversed animation finishes at 0, got ' + a5.playState);
cs = getComputedStyle(b5);
near(parseFloat(cs.opacity), 0, 0.03, 'reversed fill-both holds start value');

// ---------------------------------------------------------------------------
// 6. finished promise + onfinish
// ---------------------------------------------------------------------------
const b6 = document.createElement('div');
b6.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b6);
flush();
const a6 = b6.animate([{ opacity: 0 }, { opacity: 1 }], 200);
let promiseResolved = false;
let onfinishFired = false;
a6.finished.then((v) => { promiseResolved = (v === a6); });
a6.onfinish = () => { onfinishFired = true; };
advanceTime(300);
flush();
flush();
assert(promiseResolved, 'finished promise resolved with the animation');
assert(onfinishFired, 'onfinish fired');
assert(a6.playState === 'finished', 'a6 finished');

// finish() jumps to the end synchronously
const b6b = document.createElement('div');
b6b.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b6b);
flush();
const a6b = b6b.animate([{ opacity: 0 }, { opacity: 1 }],
                        { duration: 5000, fill: 'forwards' });
let finishNow = false;
a6b.onfinish = () => { finishNow = true; };
a6b.finish();
assert(a6b.playState === 'finished', 'finish() → finished');
assert(finishNow, 'finish() fires onfinish synchronously');
near(a6b.currentTime, 5000, 1, 'finish() seeks to end');
flush();
cs = getComputedStyle(b6b);
near(parseFloat(cs.opacity), 1, 0.02, 'finish() + fill forwards applies final value');

// ---------------------------------------------------------------------------
// 7. cancel(): AbortError rejection + oncancel + style revert + idle
// ---------------------------------------------------------------------------
const b7 = document.createElement('div');
b7.style.cssText = 'width:40px;height:10px;opacity:0.8;';
root.appendChild(b7);
flush();
const a7 = b7.animate([{ opacity: 0 }, { opacity: 1 }],
                      { duration: 1000, fill: 'forwards' });
let rejectedWithAbort = false;
let oncancelFired = false;
a7.finished.catch((e) => { rejectedWithAbort = (e && e.name === 'AbortError'); });
a7.oncancel = () => { oncancelFired = true; };
advanceTime(300);
a7.cancel();
assert(oncancelFired, 'oncancel fired synchronously');
assert(a7.playState === 'idle', 'canceled → idle');
assert(a7.currentTime === null, 'canceled → currentTime null');
flush();
assert(rejectedWithAbort, 'finished promise rejected with AbortError');
cs = getComputedStyle(b7);
near(parseFloat(cs.opacity), 0.8, 0.02, 'cancel reverts to base style');
// The finished promise is replaced after cancel — the new one is pending, not rejected.
const freshPromise = a7.finished;
assert(freshPromise instanceof Promise, 'finished promise recreated after cancel');
// play() after cancel restarts from scratch
a7.play();
assert(a7.playState === 'running', 'play() after cancel restarts');
near(a7.currentTime, 0, 20, 'restart begins at 0');
advanceTime(1500);
assert(a7.playState === 'finished', 'restarted animation finishes');

// ---------------------------------------------------------------------------
// 8. getAnimations + last-started-wins stacking
// ---------------------------------------------------------------------------
const b8 = document.createElement('div');
b8.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b8);
flush();
const s1 = b8.animate([{ opacity: 0 }, { opacity: 0.2 }],
                      { duration: 1000, easing: 'linear', fill: 'forwards' });
const s2 = b8.animate([{ opacity: 1 }, { opacity: 0.6 }],
                      { duration: 1000, easing: 'linear', fill: 'forwards' });
const list = b8.getAnimations();
assert(list.length === 2, 'getAnimations() returns both, got ' + list.length);
assert(list[0] === s1 && list[1] === s2, 'getAnimations() preserves identity + order');
assert(document.getAnimations().length >= 2, 'document.getAnimations() includes them');
advanceTime(500);
cs = getComputedStyle(b8);
near(parseFloat(cs.opacity), 0.8, 0.05, 'last-started animation wins per property');
s1.cancel();
s2.cancel();
assert(b8.getAnimations().length === 0, 'getAnimations() empty after cancel');

// ---------------------------------------------------------------------------
// 9. delay + fill backwards; per-keyframe offsets/easing; object form
// ---------------------------------------------------------------------------
const b9 = document.createElement('div');
b9.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b9);
flush();
const a9 = b9.animate([{ opacity: 0.1 }, { opacity: 0.9 }],
                      { duration: 200, delay: 300, fill: 'backwards', easing: 'linear' });
flush();
cs = getComputedStyle(b9);
near(parseFloat(cs.opacity), 0.1, 0.03, 'fill backwards applies first keyframe during delay');
advanceTime(400); // 100ms into active
cs = getComputedStyle(b9);
near(parseFloat(cs.opacity), 0.5, 0.06, 'delay offsets active phase');
advanceTime(300);

// explicit offsets: 0 → 0.8 → 1
const b9b = document.createElement('div');
b9b.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b9b);
flush();
const a9b = b9b.animate([
    { opacity: 0 },
    { opacity: 1, offset: 0.8 },
    { opacity: 0.5 }
], { duration: 1000, easing: 'linear', fill: 'forwards' });
advanceTime(400); // t=.4 → between offsets 0 and .8 → .5 of the way → opacity .5
cs = getComputedStyle(b9b);
near(parseFloat(cs.opacity), 0.5, 0.05, 'explicit offset segment 1');
advanceTime(500); // t=.9 → between .8 and 1 → half → 1 → 0.75
cs = getComputedStyle(b9b);
near(parseFloat(cs.opacity), 0.75, 0.05, 'explicit offset segment 2');
advanceTime(200);

// object-of-arrays form
const b9c = document.createElement('div');
b9c.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b9c);
flush();
const a9c = b9c.animate({ opacity: [0, 1] }, { duration: 400, easing: 'linear' });
assert(a9c.playState === 'running', 'object-form keyframes accepted');
advanceTime(200);
cs = getComputedStyle(b9c);
near(parseFloat(cs.opacity), 0.5, 0.05, 'object-form interpolates');
advanceTime(300);

// single-keyframe form animates from the base value
const b9d = document.createElement('div');
b9d.style.cssText = 'width:40px;height:10px;opacity:0.9;';
root.appendChild(b9d);
flush();
const a9d = b9d.animate([{ opacity: 0.1 }], { duration: 400, easing: 'linear', fill: 'forwards' });
advanceTime(200);
cs = getComputedStyle(b9d);
near(parseFloat(cs.opacity), 0.5, 0.05, 'single keyframe interpolates from base');
advanceTime(300);

// ---------------------------------------------------------------------------
// 10. infinite iterations + element removal mid-animation (no crash)
// ---------------------------------------------------------------------------
const b10 = document.createElement('div');
b10.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(b10);
flush();
const a10 = b10.animate([{ opacity: 0 }, { opacity: 1 }],
                        { duration: 100, iterations: Infinity });
advanceTime(1000);
assert(a10.playState === 'running', 'infinite animation still running');
let threw = false;
try { a10.finish(); } catch (e) { threw = (e && e.name === 'InvalidStateError'); }
assert(threw, 'finish() on infinite animation throws InvalidStateError');
a10.cancel();

const rm = document.createElement('div');
rm.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(rm);
flush();
const arm = rm.animate([{ opacity: 0 }, { opacity: 1 }], 500);
advanceTime(100);
rm.remove();
advanceTime(1000); // ticks against a detached (then freed) element — must not crash
flush();
assert(arm.playState === 'finished', 'animation on removed element still finishes');

// bad keyframes throw a TypeError
let badThrew = false;
try { b10.animate([{ opacity: 0, offset: 0.9 }, { opacity: 1, offset: 0.1 }], 100); }
catch (e) { badThrew = (e instanceof TypeError); }
assert(badThrew, 'non-monotonic offsets throw TypeError');

// ---------------------------------------------------------------------------
// 11. Leave animations running at script end — engine teardown must reclaim
//     records and wrappers cleanly (Debug leak asserts cover this).
// ---------------------------------------------------------------------------
const tail = document.createElement('div');
tail.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(tail);
flush();
tail.animate([{ transform: 'rotate(0deg)' }, { transform: 'rotate(360deg)' }],
             { duration: 500, iterations: Infinity });
const tail2 = document.createElement('div');
tail2.style.cssText = 'width:40px;height:10px;opacity:1;';
root.appendChild(tail2);
flush();
const atail = tail2.animate([{ opacity: 0 }, { opacity: 1 }],
                            { duration: 100, fill: 'forwards' });
advanceTime(200); // finished + filling forwards at teardown
assert(atail.playState === 'finished', 'tail fill-forwards animation finished');
