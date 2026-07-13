// <iframe> sub-documents: lifecycle, isolation, virtual time, reload, teardown.
//
// The iframe feature shipped with no tests at all, which is how two lifetime
// bugs (a leaked GPU surface, and a bad src silently embedding the host app in
// itself) sat unnoticed. This is the coverage.
//
// Every iframe here is created from JS. That only works because syncIframes()
// now runs on a DOM structure change — it used to be called once at app load,
// so a dynamically created <iframe> never got a sub-document at all, despite
// the engine header claiming otherwise.

const CHILD = 'iframe_child';

function makeFrame(src, w, h) {
    const el = document.createElement('iframe');
    el.setAttribute('src', src);
    el.style.width = (w || 200) + 'px';
    el.style.height = (h || 120) + 'px';
    document.body.appendChild(el);
    flush();          // structure change -> syncIframes() -> sub-doc built
    return el;
}

// --- creation from JS -------------------------------------------------------
let loaded = 0;
const a = document.createElement('iframe');
a.addEventListener('load', () => { loaded++; });
a.setAttribute('src', CHILD);
a.style.width = '200px';
a.style.height = '120px';
document.body.appendChild(a);
flush();

assert(loaded === 1, 'iframe fired exactly one load event, got ' + loaded);

// --- .src on a fresh iframe builds exactly ONE sub-document ------------------
// Assigning .src queues a reload AND appendChild dirties the structure, so both
// the reload drain and syncIframes() have a claim on building it. They used to
// both act: sync built the sub-doc, then the reload tore it down and rebuilt it
// — two sub-documents and two load events for one iframe.
let srcLoads = 0;
const viaSrc = document.createElement('iframe');
viaSrc.addEventListener('load', () => { srcLoads++; });
viaSrc.src = CHILD;                    // the .src setter, not setAttribute
viaSrc.style.width = '160px';
viaSrc.style.height = '100px';
document.body.appendChild(viaSrc);
flush();
assert(srcLoads === 1,
       'el.src = "app" on a fresh iframe builds ONE sub-document, got ' + srcLoads);
assert(viaSrc.capture(), 'the .src-built sub-document renders');

// --- the sub-document is real, and isolated from the host --------------------
// capture() renders the sub-doc synchronously and hands back its pixels; a null
// return means no sub-document was ever instantiated.
const shot = a.capture();
assert(shot, 'capture() returned pixels for the sub-document');
assert(shot.width > 0 && shot.height > 0,
       'captured sub-doc has a real size, got ' + shot.width + 'x' + shot.height);

// The child defines window.__ticks / #label. Neither may leak into the host
// realm — separate JSContext, separate DOM.
assert(typeof window.__ticks === 'undefined',
       'child globals do not leak into the host realm');
assert(document.querySelector('#label') === null,
       'child DOM is not reachable from the host document');

// --- virtual time reaches the sub-document's own Timers ----------------------
// The child schedules a 50ms setTimeout and a self-rescheduling rAF. Before
// tickIframes() was wired into advanceTime(), an iframe's clock never advanced
// in headless and both stayed frozen.
advanceTime(200);
flush();
const after = a.capture();
assert(after, 'sub-document still captures after advancing time');

// --- reload rebuilds the sub-document ---------------------------------------
loaded = 0;
a.reload();
flush();
assert(loaded === 1, 'reload() fired a fresh load event, got ' + loaded);
assert(a.capture(), 'sub-document captures after reload');

// --- a src that does not exist fails, and does NOT fall back to the host ------
// The parent_path() fallback (there so src="child/index.html" resolves to its
// directory) used to also catch a src naming nothing, walking up to the parent —
// which for a bare src is the HOST app's own root. A typo silently embedded the
// host document inside itself.
const bad = document.createElement('iframe');
bad.setAttribute('src', 'no-such-app-anywhere');
bad.style.width = '100px';
bad.style.height = '80px';
document.body.appendChild(bad);
flush();
assert(bad.capture() === null,
       'a src that does not exist yields no sub-document (must not fall back ' +
       'to the host app and embed it in itself)');

// --- src= swap to a good app recovers ---------------------------------------
bad.src = CHILD;
flush();
assert(bad.capture(), 'repointing a broken iframe at a real app builds a sub-doc');

// --- removal tears the sub-document down ------------------------------------
// Churn a few frames so each one gets recorded (and, windowed, gets a GPU
// surface) before being destroyed.
for (let i = 0; i < 4; i++) {
    const tmp = makeFrame(CHILD, 120, 90);
    assert(tmp.capture(), 'churned iframe #' + i + ' built a sub-document');
    advanceTime(16);
    tmp.remove();
    flush();          // structure change -> syncIframes() tears the sub-doc down
}

// A removed iframe's element no longer resolves to a sub-document.
const gone = makeFrame(CHILD, 120, 90);
assert(gone.capture(), 'iframe built before removal');
gone.remove();
flush();
assert(gone.capture() === null, 'a removed iframe has no sub-document');

// --- screenshot() replays live sub-docs (the headless surface path) ----------
// This is what creates iframe GPU surfaces in headless — on the MAIN renderer,
// not the raster thread's. ~Engine() has to release them there.
const os = require('os');
const path = require('path');
screenshot(path.join(os.tmpdir(), 'bro_test_iframe.png'));

// Leave `a` and `bad` alive and rendered at exit: teardown must release their
// surfaces on the context that made them. A fault or a leak assertion in the
// destructor chain fails this test via the exit code.
console.log('iframe test done');
