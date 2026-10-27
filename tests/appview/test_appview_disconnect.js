// How an <appview>'s mistakes reach its page (docs/app-isolation.md, step 5):
// an exception nothing caught is an `apperror` event and the app runs on; a
// job that never finishes yields, and past kNotRespondingMs the page hears
// `unresponsive` (`responding` false) and may stop it; an app the runtime
// terminates leaves an error card in its view unless the page cancels
// `terminated`, and the card goes when the app runs again.

const messages = [];
window.addEventListener('message', (e) => messages.push(e.data));
const has = (key) => messages.some((m) => m && m[key]);

const events = [];
const av = document.createElement('appview');
for (const type of ['apperror', 'unresponsive', 'responsive', 'terminated'])
    av.addEventListener(type, (e) => events.push({ type, detail: e.detail }));
const seen = (type) => events.find((e) => e.type === type);
av.setAttribute('src', 'appview_busy');
av.style.cssText = 'position: absolute; left: 0; top: 0; width: 200px; height: 100px';
document.body.appendChild(av);
flush();
runFrames(5);
assert(av.running && has('ready'), 'the app started');
assert(av.responding === true, 'a running app is responding');

// An uncaught exception: told to the page, and the app runs on.
av.contentWindow.postMessage('throw');
runFrames(3);
const err = seen('apperror');
assert(err && /busy threw on purpose/.test(err.detail.error), `the page heard the app's error (${err && err.detail.error})`);
assert(av.running, 'the app runs on after its exception');
messages.length = 0;
av.contentWindow.postMessage('spin');
let frames = 0;
while (!has('spun') && frames++ < 2000) runFrames(1);
assert(has('spun'), 'and still does its work');

// A job that never finishes: the page goes on, and hears it. (The spin may
// itself have run past kNotRespondingMs on a slow machine.)
events.length = 0;
av.contentWindow.postMessage('forever');
frames = 0;
while (!seen('unresponsive') && frames++ < 4000) runFrames(1);
assert(seen('unresponsive'), `the page heard the app stopped responding (${frames} frames)`);
assert(av.responding === false, 'responding is false');
av.stop();
runFrames(2);
assert(av.running === false && av.responding === null, 'the page stopped it');

// Terminated: the view shows the error card, the card's background where
// the app was.
const card = () => {
    const p = getPixels(190, 90, 1, 1).data;
    return p[0] === 0x2a && p[1] === 0x1d && p[2] === 0x1f;
};
av.start();
flush();
runFrames(5);
assert(av.running, 'started again');
assert(!card(), 'no card while the app runs');
click(20, 20);
runFrames(3);
assert(seen('terminated'), 'the app was terminated');
assert(card(), 'its view shows the error card');

// The card goes when it runs again.
av.start();
flush();
runFrames(5);
assert(av.running && !card(), 'start() runs it again and the card is gone');

// A page that draws its own cancels terminated: no card.
av.addEventListener('terminated', (e) => e.preventDefault());
click(20, 20);
runFrames(3);
assert(!av.running && !card(), 'a cancelled terminated leaves no card');

av.remove();
flush();
console.log('test_appview_disconnect OK');
