// bro.conf writes are cheap: set() updates memory and returns, and the
// settings file is written later, coalesced, off the page thread, atomically
// (temp file + replace). A store holding a large value (a 1500-path string
// list, as the Music app keeps) used to cost ~7 ms per set, because every set
// rewrote the whole file on the page thread. bro.conf.flush() writes what is
// pending now; engine teardown flushes too, which a second process proves.
//
// Runs on the scratch BRO_APP_HOME the test runner gives conf/* tests: the
// store is <BRO_APP_HOME>/config/settings.ini, never the user's own.

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

if (!bro.conf.available) {
    skipTest('bro.conf is compiled out of this build');
} else if (!process.env.BRO_APP_HOME) {
    skipTest('needs a scratch BRO_APP_HOME (tests/run_tests.sh sets one for conf/*)');
} else {
    const file = path.join(process.env.BRO_APP_HOME, 'config', 'settings.ini');
    const dir = path.dirname(file);
    const median = (xs) => { const s = xs.slice().sort((a, b) => a - b); return s[s.length >> 1]; };

    // A large value already in the store.
    const big = [];
    for (let i = 0; i < 1500; ++i) big.push('D:/Music/Artist ' + i + '/Album ' + (i % 37) + '/Track ' + i + '.flac');
    await bro.conf.set('perf.library.paths', big);
    assert(typeof bro.conf.flush === 'function', 'bro.conf.flush exists');
    assert(bro.conf.flush() === true, 'flush of the large value succeeds');
    assert(fs.readFileSync(file, 'utf-8').includes('Track 1499.flac'), 'the large value reached the file');

    // ---- per-set cost on the page thread ----
    const times = [];
    for (let i = 0; i < 40; ++i) {
        const t0 = perf.now();
        bro.conf.set('perf.ui.volume', i);
        times.push(perf.now() - t0);
        // read-after-set: memory is current at once
        assert(bro.conf.get('perf.ui.volume') === i, 'get after set sees ' + i);
    }
    const med = median(times);
    console.log('set with a 1500-path store: median ' + med.toFixed(3) + ' ms, max ' + Math.max(...times).toFixed(3) + ' ms');
    assert(med < 0.5, 'median set is well under the old ~7 ms: ' + med.toFixed(3) + ' ms');

    // ---- a burst of five ----
    const bursts = [];
    for (let b = 0; b < 9; ++b) {
        const t0 = perf.now();
        for (let i = 0; i < 5; ++i) bro.conf.set('perf.ui.track', 'burst-' + b + '-' + i);
        bursts.push(perf.now() - t0);
    }
    const bmed = median(bursts);
    console.log('burst of five sets: median ' + bmed.toFixed(3) + ' ms');
    assert(bmed < 2.5, 'a burst of five sets is well under the old ~35 ms: ' + bmed.toFixed(3) + ' ms');
    assert(bro.conf.get('perf.ui.track') === 'burst-8-4', 'the last burst value reads back');

    // ---- watchers in this process see the set at once (next pump) ----
    const seen = [];
    const h = bro.conf.watch('perf.ui', 'volume', (key, nv) => seen.push(nv));
    bro.conf.set('perf.ui.volume', 1234);
    advanceTime(16);
    assert(seen.length === 1 && seen[0] === 1234, 'a watcher saw the set: ' + JSON.stringify(seen));
    h.unwatch();

    // ---- flush: the file holds the last values, written atomically ----
    assert(bro.conf.flush() === true, 'flush succeeds');
    const text = fs.readFileSync(file, 'utf-8');
    assert(/volume = 1234\n/.test(text), 'the file holds the last volume');
    assert(/track = "burst-8-4"\n/.test(text), 'the file holds the last burst value');
    assert(text.includes('Track 1499.flac'), 'the large value is still in the file');
    const leftovers = fs.readdirSync(dir).filter((n) => n.includes('.tmp.'));
    assert(leftovers.length === 0, 'no temp files left beside the store: ' + leftovers.join(', '));

    // ---- another process: teardown persists, and this process hears it ----
    const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
    assert(headless, 'a bro-headless binary');
    const heard = [];
    const h2 = bro.conf.watch('perf.ui', 'child', (key, nv) => heard.push(nv));
    // A value of our own still pending when the other process's write lands
    // must survive the reload that write triggers.
    bro.conf.set('perf.ui.mine', 'kept');
    const fixture = path.resolve('tests/conf/fixtures/conf_child.js');
    const r = cp.spawnSync(headless, [bro.appDir, fixture], {
        env: { ...process.env, BRO_TEST_CONF_CHILD_N: '5' }, encoding: 'utf-8', timeout: 120000,
    });
    assert(r.status === 0, 'the child ran: status ' + r.status + ' ' + (r.stderr || '') + (r.stdout || ''));
    assert(/child = "from-child-5"\n/.test(fs.readFileSync(file, 'utf-8')),
        'the child\'s last set reached the file at its teardown');

    const end = Date.now() + 10000;
    while (Date.now() < end && !heard.includes('from-child-5')) { advanceTime(16); wallSleep(20); }
    assert(heard.includes('from-child-5'), 'this process heard the other one\'s write: ' + JSON.stringify(heard));
    assert(bro.conf.get('perf.ui.child') === 'from-child-5', 'and reads its value');
    assert(bro.conf.get('perf.ui.mine') === 'kept', 'our own value survived the reload');
    assert(bro.conf.get('perf.ui.volume') === 1234, 'and so did the rest of ours');
    h2.unwatch();
    assert(bro.conf.flush() === true, 'final flush');
    assert(/mine = "kept"\n/.test(fs.readFileSync(file, 'utf-8')), 'our value is in the file');

    console.log('test_conf_write_cost.js PASSED');
}
