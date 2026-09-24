// CSS @keyframes animations and element.animate() are one system, as on the
// web: a CSS animation is a CSSAnimation in getAnimations() (element and
// document), every layer of a comma-separated `animation` runs, script can
// pause / seek / re-time a CSS animation, and the animation* events follow
// its phase — including phase changes script causes.
//
// Before: getAnimations() listed only script animations, a comma list ran
// only its first layer, and CSS animations ran on a separate interpolator
// script could not reach.

const style = document.createElement('style');
style.textContent = `
  @keyframes mvx { from { transform: translateX(0px) } to { transform: translateX(1000px) } }
  @keyframes fade { from { opacity: 0 } to { opacity: 1 } }
  .one { animation: mvx 1000ms linear infinite; }
  .two { animation: mvx 1000ms linear, fade 2000ms linear; }
  .late { animation: fade 400ms linear 200ms; }
  .held { animation-play-state: paused; }
`;
document.head.appendChild(style);

const root = document.getElementById('root');
root.innerHTML = '<div id="a" style="width:10px;height:10px"></div>' +
                 '<div id="b" style="width:10px;height:10px"></div>' +
                 '<div id="c" style="width:10px;height:10px"></div>';
const a = document.getElementById('a');
const b = document.getElementById('b');
const c = document.getElementById('c');
flush();

function near(v, want, eps, msg) {
    assert(Math.abs(v - want) <= eps, msg + ' (expected ~' + want + ', got ' + v + ')');
}
function tx(el) {
    const t = getComputedStyle(el).transform;
    if (t === 'none' || t === '') return 0;
    let m = /translateX\((-?[\d.e+-]+)px\)/.exec(t);
    if (m) return +m[1];
    m = /matrix\(([^)]*)\)/.exec(t);
    return m ? +m[1].split(',')[4] : NaN;
}
const op = (el) => parseFloat(getComputedStyle(el).opacity);

// --- getAnimations lists the CSS animation, straight after the class change.
a.classList.add('one');
let list = a.getAnimations();
assert(list.length === 1, 'the CSS animation is listed at once, got ' + list.length);
const cssA = list[0];
assert(cssA instanceof CSSAnimation, 'it is a CSSAnimation');
assert(cssA instanceof Animation, 'which is an Animation');
assert(cssA.animationName === 'mvx', 'animationName: ' + cssA.animationName);
assert(cssA.playState === 'running', 'running: ' + cssA.playState);
assert(a.getAnimations()[0] === cssA, 'the same object on every call');
assert(document.getAnimations().indexOf(cssA) >= 0, 'document.getAnimations() lists it too');
assert(cssA.effect.target === a, 'effect.target is the element');
assert(cssA.effect.getTiming().duration === 1000, 'getTiming().duration from animation-duration');
assert(cssA.effect.getTiming().iterations === Infinity, 'getTiming().iterations from the count');
assert(cssA.effect.getKeyframes().length === 2, 'getKeyframes() from the @keyframes rule');
assert(cssA.effect.getKeyframes()[0].easing === 'linear', 'keyframe easing is the animation-timing-function');

advanceTime(250);
near(tx(a), 250, 15, 'the CSS animation runs');

// --- script control of a CSS animation.
cssA.pause();
advanceTime(300);
near(tx(a), 250, 15, 'pause() holds it');
cssA.currentTime = 600;
near(tx(a), 600, 15, 'currentTime seeks it');
// animation-play-state no longer rules an animation script has paused/played.
a.classList.add('held');
flush();
cssA.play();
advanceTime(100);
near(tx(a), 700, 15, 'play() resumes it, animation-play-state notwithstanding');
a.classList.remove('held');

// updateTiming() sticks across re-cascades of the longhands.
cssA.effect.updateTiming({ duration: 2000 });
a.style.width = '11px';  // an unrelated restyle
flush();
assert(cssA.effect.getTiming().duration === 2000, 'updateTiming() survives a restyle, got ' +
       cssA.effect.getTiming().duration);

// Removing the class cancels it: idle, oncancel, finished rejects.
let canceled = 0;
cssA.oncancel = () => canceled++;
const finished = cssA.finished;
let rejected = null;
finished.catch((e) => { rejected = e.name; });
a.classList.remove('one');
flush();
advanceTime(16);
assert(cssA.playState === 'idle', 'removing the class leaves it idle: ' + cssA.playState);
assert(canceled === 1, 'oncancel fired once, got ' + canceled);
assert(rejected === 'AbortError', 'finished rejected with AbortError, got ' + rejected);
assert(a.getAnimations().length === 0, 'and it is no longer listed');
near(tx(a), 0, 0.5, 'the element is un-animated');

// --- a comma list runs every layer, in animation-name order.
b.classList.add('two');
list = b.getAnimations();
assert(list.length === 2, 'both layers are animations, got ' + list.length);
assert(list[0].animationName === 'mvx' && list[1].animationName === 'fade',
       'composite order is animation-name order: ' + list.map((x) => x.animationName));
advanceTime(500);
near(tx(b), 500, 20, 'layer 1 (transform) runs');
near(op(b), 0.25, 0.03, 'layer 2 (opacity) runs too');

// Script animations sort after the CSS ones.
const scripted = b.animate([{ opacity: 1 }, { opacity: 1 }], 100000);
list = b.getAnimations();
assert(list.length === 3 && list[2] === scripted, 'script animation after the CSS layers');
near(op(b), 1, 0.01, 'and wins the property (it is higher in the stack)');
scripted.cancel();

// --- events follow the phase: delayed start, script-driven end and cancel.
const events = [];
for (const t of ['animationstart', 'animationend', 'animationcancel', 'animationiteration']) {
    c.addEventListener(t, (e) => events.push(e.type + ':' + e.animationName + '@' + e.elapsedTime));
}
c.classList.add('late');
flush();
advanceTime(100);
assert(events.length === 0, 'no animationstart during the delay: ' + JSON.stringify(events));
advanceTime(150);
assert(events.join() === 'animationstart:fade@0', 'animationstart once the delay ends: ' + JSON.stringify(events));
const late = c.getAnimations()[0];
late.finish();
advanceTime(16);
assert(events[1] === 'animationend:fade@0.4', 'finish() from script ends it with animationend: ' +
       JSON.stringify(events));
// A finished animation whose name is unchanged does not restart.
advanceTime(1000);
assert(events.length === 2, 'no restart: ' + JSON.stringify(events));

// Restart, then cancel() from script: animationcancel.
c.classList.remove('late');
void c.offsetWidth;
c.classList.add('late');
advanceTime(300);
const again = c.getAnimations()[0];
assert(again && again !== late, 'a fresh animation after the name went and came back');
again.cancel();
advanceTime(16);
{
    // elapsedTime: the ~100 ms of active time it had run (as of the last frame).
    const m = /^animationcancel:fade@([\d.]+)$/.exec(events[events.length - 1]);
    assert(m && +m[1] > 0.05 && +m[1] <= 0.1, 'cancel() fires animationcancel: ' + JSON.stringify(events));
}
