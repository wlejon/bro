// CSS animations and display:none, and document.getAnimations() order.
//
// display:none on an element or an ancestor cancels its CSS animations
// (animationcancel; the CSSAnimation goes idle and is no longer listed), and
// displaying it again starts them from the beginning (CSS Animations §3).
// Before: the animation's clock kept running while hidden, and it resumed
// mid-flight when shown.
//
// document.getAnimations() lists CSS animations by the tree order of their
// elements (then animation-name order), before script animations. Before:
// they came in creation order.

const style = document.createElement('style');
style.textContent = `
  @keyframes mvx { from { transform: translateX(0px) } to { transform: translateX(1000px) } }
  @keyframes fade { from { opacity: 0 } to { opacity: 1 } }
  .run { animation: mvx 1000ms linear; }
  .both { animation: fade 1000ms linear, mvx 1000ms linear; }
`;
document.head.appendChild(style);

const root = document.getElementById('root');
root.innerHTML = '<div id="wrap"><div id="a" style="width:10px;height:10px"></div></div>' +
                 '<div id="b" style="width:10px;height:10px"></div>' +
                 '<div id="c" style="width:10px;height:10px"></div>';
const wrap = document.getElementById('wrap');
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
const events = [];
for (const t of ['animationstart', 'animationend', 'animationcancel']) {
    a.addEventListener(t, (e) => events.push(e.type + ':' + e.animationName));
}

// --- the element's own display:none.
a.classList.add('run');
const first = a.getAnimations()[0];
advanceTime(300);
near(tx(a), 300, 20, 'it runs');
a.style.display = 'none';
advanceTime(32);
assert(first.playState === 'idle', 'display:none cancels it: ' + first.playState);
assert(a.getAnimations().length === 0, 'it is no longer listed');
assert(events.join() === 'animationstart:mvx,animationcancel:mvx',
       'with an animationcancel: ' + JSON.stringify(events));
advanceTime(500);
a.style.display = '';
const second = a.getAnimations()[0];
assert(second && second !== first, 'shown again, a new animation starts');
assert(second.currentTime === 0, 'from the beginning, not where the old clock got to: ' + second.currentTime);
advanceTime(100);
near(tx(a), 100, 20, 'and runs from there');

// --- an ancestor's display:none.
events.length = 0;
wrap.style.display = 'none';
assert(a.getAnimations().length === 0, 'an ancestor display:none cancels it at once');
advanceTime(32);
assert(second.playState === 'idle', 'an ancestor display:none cancels it: ' + second.playState);
assert(events.join() === 'animationstart:mvx,animationcancel:mvx' || events.join() === 'animationcancel:mvx',
       'with an animationcancel: ' + JSON.stringify(events));
assert(document.getAnimations().length === 0, 'nothing listed while hidden');
advanceTime(2000);
wrap.style.display = '';
const third = a.getAnimations()[0];
assert(third && third !== second, 'shown again, it starts afresh at once');
assert(third.currentTime === 0, 'from the beginning: ' + third.currentTime);
advanceTime(100);
assert(third.playState === 'running', 'running: ' + third.playState);
near(tx(a), 100, 20, 'the element animates again');
third.cancel();
a.classList.remove('run');
flush();

// --- document.getAnimations(): CSS animations in tree order.
c.classList.add('run');   // created first, last in the tree
flush();
b.classList.add('both');
flush();
a.classList.add('run');   // created last, first in the tree
const scripted = b.animate([{ width: '10px' }, { width: '20px' }], 1000);
const all = document.getAnimations();
const desc = all.map((x) => (x.effect.target ? x.effect.target.id : '?') + ':' +
                            (x instanceof CSSAnimation ? x.animationName : 'script'));
assert(desc.join() === 'a:mvx,b:fade,b:mvx,c:mvx,b:script',
       'tree order, then animation-name order, then script: ' + desc.join());

scripted.cancel();
root.innerHTML = '';
