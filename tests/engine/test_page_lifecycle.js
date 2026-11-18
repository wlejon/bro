// The app document's load, as a browser runs it (HTML "the end"): classic
// scripts while readyState is "loading", each its own task, so one that
// throws is reported (window.onerror, the log) and the next still runs;
// readiness "interactive" with readystatechange; the module scripts
// (deferred); DOMContentLoaded; readiness "complete" with readystatechange;
// then the window's load, which reaches window.onload even when a module
// script set it. Event handler attributes (document.onreadystatechange,
// window.onload) fire beside the listeners, in registration order. A global
// one script makes as a property of the global object (a UMD bundle's
// CodeMirror) is read by its bare name in the next.
//
// A child bro-headless runs tests/engine/page_lifecycle_app: the page throws
// at its top level, which fails its own run, so it cannot be this run's app.

const cp = require('child_process');
const path = require('path');

const exe = path.join(process.env.BRO_EXE_DIR, process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless');
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'page_lifecycle_app');
const r = cp.spawnSync(exe, [appDir, '-e', "'LIFECYCLE ' + JSON.stringify({ log: __log, errors: __errors, readyState: document.readyState })"],
                       { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');
const line = out.split('\n').find(l => l.startsWith('LIFECYCLE '));
assert(line, 'the child printed its log; output was:\n' + out);
const got = JSON.parse(line.slice('LIFECYCLE '.length));

const expected = [
    'script1 loading',
    'script2',
    'script3 object',
    'readystatechange interactive',
    'onreadystatechange interactive',
    'module interactive',
    'DOMContentLoaded interactive',
    'readystatechange complete',
    'onreadystatechange complete',
    'load complete',
    'onload set by the module',
];
assert(JSON.stringify(got.log) === JSON.stringify(expected),
       'load order\n  expected ' + JSON.stringify(expected) + '\n  got      ' + JSON.stringify(got.log));
assert(got.errors.length === 1 && got.errors[0].includes('script 2 throws'),
       'window.onerror saw the one uncaught error, got ' + JSON.stringify(got.errors));
assert(out.includes('script 2 throws'), 'the uncaught error is logged');
assert(r.status !== 0, 'a page script that threw at its top level fails the run');
assert(got.readyState === 'complete', 'readyState settles at complete');

console.log('test_page_lifecycle: OK');
