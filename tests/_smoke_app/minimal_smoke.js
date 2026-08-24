// Smoke test for a build with features compiled out (BRO_PROFILE=minimal).
//
// Two jobs. The floor every profile guarantees -- HTML parses, JS runs, layout
// measures, Canvas2D draws -- and the part that actually rots: every gated
// `bro.*` namespace must still be there whether or not its feature was built.
//
// A missing stub is invisible in the app/full profiles, because there the real
// binding is compiled and the `#else` branch is never even parsed. It shows up
// only in a build with the flag off, which is the build nobody ran: `bro.media`
// declared its stub outside its own includes and failed to compile at all, and
// that stood until someone tried the profile BUILDING.md hands to newcomers.
//
// Run: bro-headless tests/_smoke_app tests/_smoke_app/minimal_smoke.js
// (Deliberately not tests/<dir>/test_*.js -- run_tests.sh discovers those, and
// this one is about the profile, not the feature.)

// --- DOM -------------------------------------------------------------------
assert(document.body, 'document.body exists');

const el = document.createElement('div');
el.textContent = 'hello';
el.style.width = '120px';
el.style.height = '40px';
document.body.appendChild(el);

assert(el.textContent === 'hello', 'textContent round-trips');
assert(document.querySelector('div') === el, 'querySelector finds the appended node');

// A geometry read flushes layout, so this is a real pass, not just a tree walk.
const box = el.getBoundingClientRect();
assert(box.width === 120, 'laid-out width is 120, got ' + box.width);
assert(box.height === 40, 'laid-out height is 40, got ' + box.height);

// --- Canvas2D --------------------------------------------------------------
const canvas = document.createElement('canvas');
canvas.width = 32;
canvas.height = 32;
const ctx = canvas.getContext('2d');
assert(ctx, 'canvas 2d context');

ctx.fillStyle = '#ff0000';
ctx.fillRect(0, 0, 32, 32);
const px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 255 && px[1] === 0 && px[2] === 0 && px[3] === 255,
       'fillRect wrote opaque red, got [' + Array.from(px).join(', ') + ']');

// --- Availability stubs ----------------------------------------------------
// Every namespace behind a BRO_WITH_* flag. The contract apps rely on is that
// `bro.<name>` is always THERE: compiled in it is the real binding, compiled
// out feature_stubs.cpp installs a throwing Proxy that reports
// `available === false`. What must never happen is the namespace being absent,
// because then feature detection is a ReferenceError instead of an answer.
//
// (`available` is not asserted true for a compiled-in namespace -- most only
// grow the property when stubbed. Physics and Mesh are global classes, not
// `bro.*` namespaces, and a build without them just does not define the global,
// so there is nothing to check for them here.)
const GATED = [
  'media', 'net', 'flora', 'ai', 'gizmo', 'impostor',
  'lm', 'stt', 'tts', 'diar', 'rave', 'wake', 'kws', 'sense', 'gesture',
  'listen', 'vision', 'diffusion', 'tensor', 'gpu', 'triposplat', 'motion',
];

// bro.gpu is the exception, on purpose: it is a backend probe, not a feature.
// `available: false` with `backend: 'cpu'` is a real answer to a real question,
// so it stays a plain object rather than becoming a throwing Proxy.
const PROBES = ['gpu'];

const absent = GATED.filter(n => !bro[n]);
assert(absent.length === 0, 'gated namespaces with no stub at all: ' + absent.join(', '));

// A compiled-out namespace must explain itself rather than answer undefined.
const off = GATED.filter(n => bro[n].available === false && !PROBES.includes(n));
for (const n of off) {
  let threw = null;
  try { bro[n].somethingThisBuildDoesNotHave(); } catch (e) { threw = e; }
  assert(threw, 'bro.' + n + ' reports unavailable but calling into it did not throw');
  assert(/compiled without BRO_WITH_/.test(String(threw.message)),
         'bro.' + n + ' threw without naming its flag: ' + threw.message);
}

console.log('ok - dom, layout, canvas2d');
console.log('ok - ' + GATED.length + ' gated namespaces present');
console.log('ok - ' + off.length + ' compiled out, each throwing by flag: ' + (off.join(', ') || '(none)'));
