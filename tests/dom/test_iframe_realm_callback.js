// The other way from test_iframe_realm_call.js: a function of the parent's
// realm the child calls (one passed in, a listener the parent put on the
// child's element, a promise continuation) runs in the parent's realm, against
// the parent's document and globals, as it does in a browser; and the child's
// own continuations run in the child, wherever the microtask checkpoint is.
// Sub-document realms share one global object and are entered by swapping it
// (bronze_host: enterRealmScope, the document global, the current document).
// A callback ran in whichever realm called it: the child's document, the
// child's globals. A promise job ran in the checkpoint's realm, the app's: a
// child's `then` read the parent's document. Now a function the parent passes
// in is wrapped to enter the parent's realm, a listener runs in the realm that
// registered it, and a promise job in the realm that subscribed it (bronze's
// job-context hooks).

document.title = 'Parent title';
globalThis.parentOnly = 'parent';
const frame = document.createElement('iframe');
frame.setAttribute('src', 'realm_child');
frame.style.cssText = 'width: 200px; height: 100px';
document.body.appendChild(frame);
flush();
const cw = frame.contentWindow;
assert(cw && typeof cw.callBack === 'function', 'the child loaded and its functions are on its contentWindow');
const parentBoxes = document.querySelectorAll('.box').length;

const where = () => [document.title, typeof globalThis.parentOnly, document.querySelectorAll('.box').length].join('|');
const parentHere = ['Parent title', 'string', parentBoxes].join('|');

// A function passed in: the child calls it, and it runs here.
let seen = null;
const r = cw.callBack(function (x) { seen = where(); return x * 2; }, 7);
assert(seen === parentHere, `a function passed to the child runs in the parent's realm (${seen})`);
assert(r === 14, 'its argument and result pass through');
assert(document.title === 'Parent title' && globalThis.parentOnly === 'parent', 'the parent realm is back after');

// A throw out of it reaches the parent caller through the child.
let caught = null;
try { cw.callBack(function () { throw new Error('thrown in the parent'); }); } catch (e) { caught = e; }
assert(caught && String(caught.message).includes('thrown in the parent'), `the callback's throw reaches the caller (${caught})`);
assert(document.title === 'Parent title' && globalThis.parentOnly === 'parent', 'the parent realm is back after a throw');

// A listener the parent puts on the child's element runs in the parent; the
// child's own listener on it runs in the child.
let heard = null;
cw.document.getElementById('b').addEventListener('click', function () { heard = where(); });
assert(cw.clickB() === 'Realm child', 'the child clicks its button in its own realm');
assert(heard === parentHere, `the parent's listener on the child's element runs in the parent (${heard})`);
assert(cw.readOwnHeard() === 'Realm child', `the child's own listener runs in the child (${cw.readOwnHeard()})`);

// One function passed twice is one function there: the child removes the
// listener the parent's function became.
let count = 0;
const counted = function () { count++; };
cw.listen(counted);
cw.clickB();
cw.unlisten(counted);
cw.clickB();
assert(count === 1, `a function passed twice is the same function in the child (heard ${count} times)`);

// Promise continuations: the child's runs in the child, the parent's in the parent.
cw.thenHere();
let got = null, gotWhere = null;
cw.resolveLater().then(function (t) { got = t; gotWhere = where(); });
let awaited = null;
(async function () {
  const t = await cw.resolveLater();
  awaited = t + '/' + where();
})();
for (let i = 0; i < 4 && awaited === null; i++) { advanceTime(20); flush(); }
assert(cw.readThenSaw() === 'Realm child', `a child's then continuation runs in the child (${cw.readThenSaw()})`);
assert(got === 'Realm child', `the child's promise resolved in the child (${got})`);
assert(gotWhere === parentHere, `the parent's then on it runs in the parent (${gotWhere})`);
assert(awaited === 'Realm child/' + parentHere, `an await in the parent resumes in the parent (${awaited})`);
console.log('iframe realm callback: the parent\'s callbacks ran in the parent, the child\'s continuations in the child');
