// F5 belongs to the edit loop. An app whose bro.json says `"watch": false`
// is not reloaded on a source change, so the engine's `system_reload_app`
// action is not bound to F5 there and the key reaches the page; an app that
// is watched keeps F5 as its reload key.
//
// Each case runs in a child bro-headless with a scratch BRO_APP_HOME, so the
// settings it reads and writes are not the user's.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary to run the fixtures');

const scratch = path.join(os.tmpdir(), 'bro_f5_test_' + process.pid + '_' + Date.now());
fs.mkdirSync(scratch, { recursive: true });

function probe(appDir, home, steps) {
    const code = steps + ';console.log("RESULT:" + JSON.stringify(globalThis.__r))';
    const env = { ...process.env, BRO_APP_HOME: path.join(scratch, home) };
    delete env.BRO_WATCH;
    const out = cp.execFileSync(headless, [appDir, '-e', code], { env, encoding: 'utf8' });
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the probe printed a result, got: ' + out.slice(-2000));
    return JSON.parse(line.slice(line.indexOf('RESULT:') + 7));
}

const SDLK_F5 = 0x4000003E;
const SDL_SCANCODE_F5 = 62;
const steps = 'keyDown(' + SDLK_F5 + ', ' + SDL_SCANCODE_F5 + ', 0); keyUp(' + SDLK_F5 + ', ' + SDL_SCANCODE_F5 + ', 0); advanceTime(32);' +
    'globalThis.__r = { keys: bro.settings.getActionKeys("system_reload_app"),' +
    ' action: bro.settings.getKeyAction("F5"), seen: window.keysSeen || null }';

// "watch": false — F5 is the app's.
{
    const r = probe(path.resolve('tests/app/fixtures/unwatched_app'), 'unwatched', steps);
    assert(Array.isArray(r.keys) && r.keys.length === 0,
        'system_reload_app is unbound in an unwatched app, got ' + JSON.stringify(r.keys));
    assert(r.action === null, 'F5 maps to no engine action, got ' + JSON.stringify(r.action));
    assert(Array.isArray(r.seen) && r.seen.indexOf('F5') >= 0,
        'the page received F5, saw ' + JSON.stringify(r.seen));
}

// A watched app (the default) keeps F5 for the edit loop.
{
    const r = probe(path.resolve('tests/app/fixtures/manifest_app'), 'watched',
        'globalThis.__r = { keys: bro.settings.getActionKeys("system_reload_app") }');
    assert(Array.isArray(r.keys) && r.keys.indexOf('F5') >= 0,
        'system_reload_app is F5 in a watched app, got ' + JSON.stringify(r.keys));
}

fs.rmSync(scratch, { recursive: true, force: true });
console.log('test_f5_unwatched.js PASSED');
