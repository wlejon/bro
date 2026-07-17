// location.reload() inside an <iframe> sub-document reloads that iframe —
// the same deferred teardown/rebuild as the host calling iframe.reload():
// the sub-doc realm is destroyed, the app re-parsed and re-run from src, and
// the host sees another 'load' event on the element.
//
// The child app (tests/test_app/reload_child) counts its runs in
// localStorage: run 1 schedules location.reload() 30ms in; run 2 settles.
// That gives three behavioral assertions from the host, which cannot see the
// sub-doc's DOM at all: the load event fires again (reload happened), it
// fires exactly twice (the rebuilt realm re-ran the script fresh and read
// runs=2 — a leaked realm would loop), and the sub-doc still renders.

const fs = require('fs');
const path = require('path');

const CHILD = 'reload_child';
const storePath = path.join(process.env.BRO_APP_DIR, CHILD, '.storage.json');

// Scrub the child's persisted run counter so a previous suite run can't leak
// a stale count into this one.
try { fs.unlinkSync(storePath); } catch (e) {}

let loads = 0;
const el = document.createElement('iframe');
el.addEventListener('load', () => { loads++; });
el.setAttribute('src', CHILD);
el.style.width = '200px';
el.style.height = '120px';
document.body.appendChild(el);
flush();

assert(loads === 1, 'initial load fired once, got ' + loads);
assert(el.capture(), 'sub-document renders before reload');

// Let the child's 30ms timer fire — it calls location.reload() in ITS realm.
// The reload is queued (never re-entrant teardown of the calling realm) and
// drained at the engine's safe point, which flush() reaches in headless.
advanceTime(100);
flush();
assert(loads === 2,
       'sub-doc location.reload() rebuilt the iframe (loads=' + loads + ')');
assert(el.capture(), 'reloaded sub-document renders');

// The rebuilt run read runs=2 and must NOT re-request a reload: more time
// must not produce more loads. This is what proves the realm was fresh —
// a realm whose script did not re-execute would still be on run 1's path.
advanceTime(300);
flush();
assert(loads === 2, 'no reload loop after the fresh run (loads=' + loads + ')');

// Calling reload twice in one turn coalesces — still exactly one more load.
// Drive it via the host-side el.reload() twice for the queued-twice case...
el.reload();
el.reload();
flush();
assert(loads === 3, 'two same-frame reload requests coalesce (loads=' + loads + ')');

// Leave no stale counter behind for the next suite run.
try { fs.unlinkSync(storePath); } catch (e) {}

console.log('iframe location.reload OK');
