// A page's subresources that cannot be had, as Chromium treats them: a
// classic <script src> that is missing, or refused by the app's permissions
// (no `net`), fires `error` at its element (a trusted Event that neither
// bubbles nor cancels) in place of running, and the next script runs; one that
// loads fires `load` after it ran. A <link rel=stylesheet> fires `load` or
// `error` the same way, the page going on without the sheet. Event handler
// content attributes (onerror="...", onclick="...") run with `this` the
// element and `event` the event; `return false` cancels; one that does not
// compile is reported and skipped.
//
// A child bro-headless runs tests/engine/subresource_errors_app (a missing
// file is an error in its log, so it cannot be this run's app).

const cp = require('child_process');
const path = require('path');

const exe = path.join(process.env.BRO_EXE_DIR, process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless');
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'subresource_errors_app');
const probe = "document.getElementById('b').click();" +
    "document.getElementById('bad').click();" +
    "const ev = new MouseEvent('click', {bubbles: true, cancelable: true});" +
    "document.getElementById('a').dispatchEvent(ev);" +
    "__log.push('a prevented ' + ev.defaultPrevented + ' ' + location.hash);" +
    "__log.push('onclick property ' + typeof document.getElementById('b').onclick);" +
    "'SUBRESOURCE ' + JSON.stringify(__log)";
const r = cp.spawnSync(exe, [appDir, '-e', probe], { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');
const line = out.split('\n').find(l => l.startsWith('SUBRESOURCE '));
assert(line, 'the child printed its log; output was:\n' + out);
const got = JSON.parse(line.slice('SUBRESOURCE '.length));

const expected = [
    'capture error SCRIPT',
    'js error missing false false true',
    'capture error SCRIPT',
    'js error refused',
    'ok.js ran',
    'js load true',
    'inline after',
    'css load',
    'capture error LINK',
    'css error missing error false',
    'capture error LINK',
    'css error refused',
    'onclick true click',
    'a prevented true ',
    'onclick property function',
];
assert(JSON.stringify(got) === JSON.stringify(expected),
       'the events, in order:\n  got      ' + JSON.stringify(got) + '\n  expected ' + JSON.stringify(expected));
assert(/onclick attribute/.test(out), 'the handler that does not compile is reported');

console.log('test_subresource_errors: OK');
