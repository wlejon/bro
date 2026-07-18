// matchMedia realm lifecycle: per-realm evaluation inside an <iframe>
// (queries evaluate against the iframe's own content box, not the host
// viewport), and teardown safety — iframe removal, iframe reload, and
// top-level location.reload() with live listener-bearing MediaQueryLists.
// The listening lists are strong-pinned by the engine so change delivery
// works after the app drops its references; every teardown path must release
// those pins (no crash, and no QuickJS leak assert under a Debug binary).
//
// Deliberately does NOT touch bro.settings: engine-group tests run in
// parallel with the chained settings/style groups that own the persisted
// .bro_settings.json. Media changes here come from resize() only.

const CHILD = 'mql_child';
const W = window.innerWidth;
const H = window.innerHeight;
assert(W > 500, 'host viewport is wide enough for the realm-scoping child asserts');

function centerPixel(shot) {
  const i = ((Math.floor(shot.height / 2) * shot.width) + Math.floor(shot.width / 2)) * 4;
  return { r: shot.data[i], g: shot.data[i + 1], b: shot.data[i + 2] };
}

// --- per-realm evaluation ----------------------------------------------------
// The child paints green only if matchMedia in ITS realm evaluates against
// the 200x120 iframe box (and correctly fails a host-sized min-width).
const el = document.createElement('iframe');
el.setAttribute('src', CHILD);
el.style.width = '200px';
el.style.height = '120px';
document.body.appendChild(el);
flush();

let shot = el.capture();
assert(shot, 'sub-document rendered');
let px = centerPixel(shot);
assert(px.g > 150 && px.r < 60,
       `iframe realm matchMedia evaluates against its own box (green), got rgb(${px.r},${px.g},${px.b})`);

// --- iframe reload with live pinned MQLs -------------------------------------
// reload() tears down the sub-doc realm (DomBindings::cleanup releases the
// pins) and rebuilds it fresh — which re-registers new pinned lists.
el.reload();
flush();
shot = el.capture();
assert(shot, 'reloaded sub-document rendered');
px = centerPixel(shot);
assert(px.g > 150 && px.r < 60,
       `reloaded realm re-evaluates cleanly (green), got rgb(${px.r},${px.g},${px.b})`);

// --- iframe removal with live pinned MQLs ------------------------------------
document.body.removeChild(el);
flush();
advanceTime(1200); // periodic GC sweeps the dead realm's wrappers

// Drive the all-realms delivery walk after the teardown: a stale registry
// entry or dangling pin would crash or assert here.
resize(W - 50, H);
resize(W, H);
flush();

// The host realm still works.
assert(matchMedia(`(width: ${W}px)`).matches === true, 'host realm matchMedia alive after teardown');

// --- top-level location.reload() with live pinned MQLs -----------------------
// Same child-process pattern as test_location_reload_toplevel.js: the app
// realm can't observe its own reload, so a second bro-headless runs an app
// that pins listening MQLs and reloads itself; verify.js then asserts the
// second realm's matchMedia works and drives delivery over the swapped realm.
const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'mql_reload_app');
const verify = path.join(appDir, 'verify.js');

const r = cp.spawnSync(exe, [appDir, verify], { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'child bro-headless exited ' + r.status + '\n--- child output ---\n' + out);
assert(out.includes('MQL_RELOAD_OK'),
       'child verified matchMedia across the reload; output was:\n' + out);

console.log('matchMedia teardown OK');
