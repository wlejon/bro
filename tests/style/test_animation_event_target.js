// An Animation is an EventTarget (Web Animations; Chromium): finish, cancel
// and remove reach addEventListener listeners as well as the on<type>
// handler (first), with `once` honoured and currentTime on the event.
// Shoelace's animateTo resolves on animation.addEventListener('finish');
// bro's Animation had no addEventListener and every open/close threw.

const el = document.createElement('div');
document.body.appendChild(el);
flush();

const a = el.animate([{ opacity: 0 }, { opacity: 1 }], 100);
assert(a instanceof EventTarget, 'an Animation is an EventTarget');
assert(typeof a.addEventListener === 'function', 'addEventListener exists');

const log = [];
a.onfinish = (e) => log.push('on:' + e.type);
a.addEventListener('finish', (e) => log.push('listener:' + (e.target === a) + ':' + (typeof e.currentTime)), { once: true });
a.finish();
await a.finished;
advanceTime(16);
assert(log.join(' ') === 'on:finish listener:true:number', 'handler then listener, got ' + log.join(' '));

log.length = 0;
a.play();
a.finish();
await a.finished;
advanceTime(16);
assert(log.join(' ') === 'on:finish', 'a once listener does not run again, got ' + log.join(' '));

const b = el.animate([{ opacity: 0 }, { opacity: 1 }], 1000);
let cancelled = 0;
const onCancel = () => { cancelled++; };
b.addEventListener('cancel', onCancel);
b.cancel();
assert(cancelled === 1, 'cancel reaches a listener');
b.removeEventListener('cancel', onCancel);
b.play();
b.cancel();
assert(cancelled === 1, 'a removed listener does not');

// The promise form Shoelace uses.
const c = el.animate([{ opacity: 0 }, { opacity: 1 }], 50);
let resolved = false;
new Promise((resolve) => {
    c.addEventListener('cancel', resolve, { once: true });
    c.addEventListener('finish', resolve, { once: true });
}).then(() => { resolved = true; });
for (let i = 0; i < 10 && !resolved; i++) { advanceTime(16); await Promise.resolve(); }
assert(resolved, 'the finish listener resolved the animation promise');

document.body.removeChild(el);
