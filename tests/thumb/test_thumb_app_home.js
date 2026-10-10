// bro.thumb's cache follows BRO_APP_HOME, as bro.conf's store and the app's
// own directories do: a test (or a scratch profile) that sets it writes its
// previews under <home>/cache/thumbnails, never into the user's real
// %LOCALAPPDATA%\thumbnails or ~/.cache/thumbnails.
//
// The run happens in a child bro-headless with a scratch BRO_APP_HOME, so
// this test itself leaves nothing in the user's cache either.

const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

if (!bro.thumb.available) {
    skipTest('bro.thumb is compiled out of this build');
} else {
    const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
    assert(headless, 'a bro-headless binary to run the child');

    const scratch = path.join(os.tmpdir(), 'bro_thumb_home_' + process.pid + '_' + Date.now());
    const home = path.join(scratch, 'home');
    fs.mkdirSync(home, { recursive: true });

    // A real picture to make a preview of.
    const src = path.join(scratch, 'picture.png');
    screenshot(src);
    assert(fs.existsSync(src), 'the source picture was written');

    const code = 'const r = bro.thumb.getSync(' + JSON.stringify(src) + ', { size: "normal" });' +
        'console.log("RESULT:" + JSON.stringify({ base: bro.thumb.getBaseDir(),' +
        ' path: bro.thumb.getThumbnailPath(' + JSON.stringify(src) + ', { size: "normal" }),' +
        ' made: !!r }))';
    const env = { ...process.env, BRO_APP_HOME: home };
    const out = cp.execFileSync(headless, [path.resolve('tests/test_app'), '-e', code], { env, encoding: 'utf8' });
    const line = out.split(/\r?\n/).find((l) => l.indexOf('RESULT:') >= 0);
    assert(line, 'the child printed a result, got: ' + out.slice(-2000));
    const r = JSON.parse(line.slice(line.indexOf('RESULT:') + 7));

    const norm = (p) => path.resolve(p).replace(/\\/g, '/').toLowerCase();
    const want = norm(path.join(home, 'cache', 'thumbnails'));
    assert(norm(r.base) === want, 'the cache is <BRO_APP_HOME>/cache/thumbnails, got ' + r.base);
    assert(norm(r.path).startsWith(want + '/'), 'a thumbnail path lies under it, got ' + r.path);
    assert(r.made === true, 'the preview was made');
    assert(fs.existsSync(r.path), 'and written there: ' + r.path);

    fs.rmSync(scratch, { recursive: true, force: true });
    console.log('test_thumb_app_home.js PASSED');
}
