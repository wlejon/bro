// An <appview> runs in time slices on a fiber of its own, so another app
// cannot stall its host page (docs/appview-api.js): a long job
// yields at its slice and finishes over later frames, and an app that does not
// return where its page waits on it (an event) is stopped, with the reason, as
// a `terminated` event at the element.

const messages = [];
window.addEventListener('message', (e) => messages.push(e.data));
const has = (key) => messages.some((m) => m && m[key]);

const av = document.createElement('appview');
let terminated = null;
av.addEventListener('terminated', (e) => {
  terminated = e.detail;
});
av.setAttribute('src', 'appview_busy');
av.style.cssText = 'position: absolute; left: 0; top: 0; width: 200px; height: 100px';
document.body.appendChild(av);
flush();
runFrames(5);
assert(av.running && has('ready'), 'the app started');

// The long job: delivered, it outlasts its slice and yields, so the frame
// that started it returns with the job unfinished; later frames finish it.
av.contentWindow.postMessage('spin');
runFrames(1);
assert(!has('spun'), 'the long job yielded instead of holding the frame');
let frames = 1;
while (!has('spun') && frames < 2000) {
  runFrames(1);
  frames++;
}
assert(has('spun'), `the long job finished slice by slice (${frames} frames)`);
assert(av.running, 'and the app runs on');

// The click listener never returns, and the dispatch waits on it: past the
// budget the app is stopped, the page carries on, and it hears why.
const r = av.getBoundingClientRect();
click(r.x + 20, r.y + 20);
runFrames(2);
assert(terminated !== null, 'the page heard that its app was stopped');
assert(/did not return/.test(terminated.reason), `the reason says what it did (${terminated.reason})`);
assert(av.running === false, 'the stopped app is not running');
assert(av.contentWindow === null, 'and has no window');

// The page may start it again.
messages.length = 0;
av.start();
flush();
runFrames(5);
assert(av.running && has('ready'), 'start() runs it again');

av.remove();
flush();
console.log('test_appview_slices OK');
