// A running CSS transition is a Web Animation, as on the web: a CSSTransition
// in getAnimations() (element and document) that script can pause, seek,
// finish and cancel, with the transition* events following its phase —
// including phase changes script causes. A retarget replaces it with a new
// CSSTransition (a reversal shortened per CSS Transitions §3.1), and
// display:none cancels it.
//
// Before: transitions ran on their own interpolator, invisible to script;
// getAnimations() never listed them and transitionrun/cancel never fired.

const root = document.getElementById('root');
root.innerHTML = '<div id="a" style="width:10px;height:10px;opacity:1;transition:opacity 1000ms linear"></div>' +
                 '<div id="b" style="width:10px;height:10px;opacity:1;transition:opacity 400ms linear 200ms"></div>';
const a = document.getElementById('a');
const b = document.getElementById('b');
flush();

function near(v, want, eps, msg) {
    assert(Math.abs(v - want) <= eps, msg + ' (expected ~' + want + ', got ' + v + ')');
}
const op = (el) => parseFloat(getComputedStyle(el).opacity);
// The elapsedTime of the last `type` event, or NaN.
function elapsedOf(type) {
    for (let i = events.length - 1; i >= 0; i--) {
        const m = new RegExp('^\\w+:' + type + ':opacity@([\\d.]+)$').exec(events[i]);
        if (m) return +m[1];
    }
    return NaN;
}
const events = [];
for (const t of ['transitionrun', 'transitionstart', 'transitionend', 'transitioncancel']) {
    for (const el of [a, b]) {
        el.addEventListener(t, (e) => events.push(el.id + ':' + e.type + ':' + e.propertyName + '@' + e.elapsedTime));
    }
}

// --- the transition is listed straight after the style change.
a.style.opacity = '0';
let list = a.getAnimations();
assert(list.length === 1, 'the running transition is listed at once, got ' + list.length);
const tr = list[0];
assert(tr instanceof CSSTransition, 'it is a CSSTransition');
assert(tr instanceof Animation, 'which is an Animation');
assert(!(tr instanceof CSSAnimation), 'and not a CSSAnimation');
assert(tr.transitionProperty === 'opacity', 'transitionProperty: ' + tr.transitionProperty);
assert(tr.playState === 'running', 'running: ' + tr.playState);
assert(a.getAnimations()[0] === tr, 'the same object on every call');
assert(document.getAnimations().indexOf(tr) >= 0, 'document.getAnimations() lists it too');
assert(tr.effect.target === a, 'effect.target is the element');
assert(tr.effect.getTiming().duration === 1000, 'getTiming().duration from transition-duration');
const kfs = tr.effect.getKeyframes();
assert(kfs.length === 2 && +kfs[0].opacity === 1 && +kfs[1].opacity === 0,
       'keyframes run from the old value to the new: ' + JSON.stringify(kfs));

advanceTime(250);
near(op(a), 0.75, 0.03, 'the transition runs');
assert(events.join() === 'a:transitionrun:opacity@0,a:transitionstart:opacity@0',
       'transitionrun then transitionstart: ' + JSON.stringify(events));

// --- script control.
tr.pause();
advanceTime(300);
near(op(a), 0.75, 0.03, 'pause() holds it');
tr.currentTime = 600;
near(op(a), 0.4, 0.03, 'currentTime seeks it');
tr.play();
advanceTime(100);
near(op(a), 0.3, 0.03, 'play() resumes it');

// finish(): transitionend, the end value, and no new transition from the
// value it last showed.
events.length = 0;
tr.finish();
advanceTime(16);
assert(events.join() === 'a:transitionend:opacity@1', 'finish() ends it: ' + JSON.stringify(events));
near(op(a), 0, 0.001, 'the element shows the end value');
assert(tr.playState === 'finished', 'finished: ' + tr.playState);
assert(a.getAnimations().length === 0, 'a finished transition is not listed');
advanceTime(100);
assert(a.getAnimations().length === 0, 'and nothing restarts');

// --- cancel() from script: transitioncancel, the end value applies.
events.length = 0;
a.style.opacity = '1';
const tr2 = a.getAnimations()[0];
assert(tr2 && tr2 !== tr, 'a new change is a new CSSTransition');
advanceTime(300);
tr2.cancel();
advanceTime(16);
assert(tr2.playState === 'idle', 'cancel() leaves it idle');
near(elapsedOf('transitioncancel'), 0.3, 0.02, 'cancel() fires transitioncancel: ' + JSON.stringify(events));
near(op(a), 1, 0.001, 'the element shows the end value');
assert(a.getAnimations().length === 0, 'nothing is listed or restarted');

// --- retarget mid-flight: the old transition is cancelled, a new one runs
// from where it had got to; reversing is shortened to the time it had run.
events.length = 0;
a.style.opacity = '0';
const out = a.getAnimations()[0];
advanceTime(250);
a.style.opacity = '0.5';
const mid = a.getAnimations()[0];
assert(mid !== out && out.playState === 'idle', 'retarget cancels the old transition');
near(+mid.effect.getKeyframes()[0].opacity, 0.75, 0.03, 'the new one starts where the old had got to');
assert(mid.effect.getTiming().duration === 1000, 'a plain retarget takes the full duration');
advanceTime(200);
near(elapsedOf('transitioncancel'), 0.25, 0.02, 'with a transitioncancel: ' + JSON.stringify(events));
a.style.opacity = '0.75';
// 0.75 is the reversing-adjusted start of `mid`: this is a reversal.
const back = a.getAnimations()[0];
near(back.effect.getTiming().duration, 200, 20, 'a reversal takes only as long as it had run');
advanceTime(250);
near(op(a), 0.75, 0.001, 'and arrives');
assert(a.getAnimations().length === 0, 'then it is done');

// --- transition-property no longer naming it cancels it.
a.style.opacity = '0.2';
const dropped = a.getAnimations()[0];
advanceTime(100);
a.style.transition = 'none';
flush();
assert(dropped.playState === 'idle', 'transition: none cancels the running transition');
near(op(a), 0.2, 0.001, 'the end value applies');

// --- delay: transitionrun at once, transitionstart when the delay ends.
events.length = 0;
b.style.opacity = '0';
flush();
advanceTime(100);
assert(events.join() === 'b:transitionrun:opacity@0', 'only transitionrun during the delay: ' + JSON.stringify(events));
near(op(b), 1, 0.001, 'the start value holds through the delay');
advanceTime(200);
assert(events.join() === 'b:transitionrun:opacity@0,b:transitionstart:opacity@0',
       'transitionstart after it: ' + JSON.stringify(events));

// --- display:none cancels a running transition.
events.length = 0;
const hiddenTr = b.getAnimations()[0];
root.style.display = 'none';
advanceTime(32);
assert(hiddenTr.playState === 'idle', 'an ancestor display:none cancels it: ' + hiddenTr.playState);
assert(events.length === 1 && events[0].indexOf('b:transitioncancel:opacity@') === 0,
       'with a transitioncancel: ' + JSON.stringify(events));
root.style.display = '';
advanceTime(32);
assert(b.getAnimations().length === 0, 'showing it again does not restart it');
near(op(b), 0, 0.001, 'it shows its end value');

// --- composite order: transitions, then CSS animations, then script.
const style = document.createElement('style');
style.textContent = '@keyframes spin { from { transform: rotate(0deg) } to { transform: rotate(360deg) } }';
document.head.appendChild(style);
b.style.animation = 'spin 1000ms linear infinite';
const scripted = a.animate([{ width: '10px' }, { width: '20px' }], 1000);
a.style.transition = 'opacity 1000ms linear';
flush();
a.style.opacity = '0.9';
const all = document.getAnimations();
assert(all.length === 3, 'three animations: ' + all.length);
assert(all[0] instanceof CSSTransition && all[1] instanceof CSSAnimation && all[2] === scripted,
       'document order: transition, CSS animation, script');
scripted.cancel();
b.style.animation = '';
advanceTime(1100);

// --- a value an animation drives, or drops on ending, is no style change.
assert(a.getAnimations().length === 0, 'a is idle: ' + a.getAnimations().length);
const pulse = a.animate([{ opacity: 0.1 }, { opacity: 0.3 }], 200);
advanceTime(100);
assert(a.getAnimations().length === 1 && a.getAnimations()[0] === pulse,
       'a running animation starts no transition');
advanceTime(150);
assert(a.getAnimations().length === 0, 'nor does its end: ' +
       a.getAnimations().map((x) => x.constructor.name));
near(op(a), 0.9, 0.001, 'the base value is back at once');
const pulse2 = a.animate([{ opacity: 0.1 }, { opacity: 0.3 }], 200);
advanceTime(50);
pulse2.cancel();
advanceTime(16);
assert(a.getAnimations().length === 0, 'nor does cancel()');
near(op(a), 0.9, 0.001, 'the base value is back at once after cancel()');
a.style.transition = 'none';

root.innerHTML = '';
