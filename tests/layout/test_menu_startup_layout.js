// A synchronous bro.menu.show() at startup must lay the document out exactly
// as a deferred one does.
//
// Showing the menu bar changes the reserved top inset, so bro.menu.show()
// re-runs the resize path: resolveStyles() + performLayout() on the app
// document. Called from a startup script, that happens BEFORE the engine
// attaches the replaced-element controls (engine::ensureReplacedElements), and
// LayoutNodeAdapter::intrinsicSize() has no answer for an <input>/<select>
// without one — so every text field and select was laid out as an empty box
// (padding + border, no line of content: 28px tall became 14px, and a field
// styled to 420px kept its intrinsic width). Nothing invalidated those boxes
// when the controls did arrive, so they stayed wrong for the life of the app
// unless something else happened to dirty that row. It bit every workshop lab
// that installs its menu synchronously at startup.
//
// Why child processes: the ordering under test is the app's own startup, which
// has already happened by the time a test script runs. So two bro-headless
// children are launched over two app dirs that differ only in whether the
// bro.menu.show() call is synchronous, and the same measure.js asserts the
// CSS-specified boxes in each. This test then compares the two runs
// element-by-element, which catches a regression that made BOTH wrong.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const layoutDir = path.join(process.env.BRO_APP_DIR, '..', 'layout');
const syncApp = path.join(layoutDir, 'menu_startup_app');
const deferApp = path.join(layoutDir, 'menu_startup_defer_app');
const measure = path.join(syncApp, 'measure.js');

function run(appDir, label) {
  const r = cp.spawnSync(exe, [appDir, measure], { encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  assert(r.status === 0,
         label + ' child bro-headless exited ' + r.status +
         '\n--- child output ---\n' + out);
  const line = out.split(/\r?\n/).find((l) => l.includes('MEASURE_JSON '));
  assert(line, label + ' child printed no measurements; output was:\n' + out);
  return JSON.parse(line.slice(line.indexOf('MEASURE_JSON ') + 13));
}

const sync = run(syncApp, 'synchronous-menu');
const defer = run(deferApp, 'deferred-menu');

const fmt = (b) => b.w.toFixed(2) + 'x' + b.h.toFixed(2) +
                   ' (offset ' + b.ow + 'x' + b.oh + ')';

for (const id of Object.keys(defer)) {
  const a = sync[id], b = defer[id];
  assert(a, 'the synchronous run measured #' + id);
  assert(Math.abs(a.w - b.w) < 0.01 && Math.abs(a.h - b.h) < 0.01,
         '#' + id + ' matches the deferred layout: sync ' + fmt(a) +
         ' vs deferred ' + fmt(b));
  assert(a.ow === b.ow && a.oh === b.oh,
         '#' + id + ' offsetWidth/Height match: sync ' + fmt(a) +
         ' vs deferred ' + fmt(b));
}

console.log('menu-at-startup layout matches the deferred layout for ' +
            Object.keys(defer).length + ' elements');
