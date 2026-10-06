// Headless test for bro.apps
assert(typeof bro.apps === 'object', 'bro.apps namespace exists');
if (!bro.apps.available) {
    skipTest('bro.apps is compiled out of this build');
} else {
    // 1. Capabilities
    const caps = bro.apps.getCapabilities();
    assert(typeof caps === 'object', 'caps is an object');
    assert(typeof caps.launcher === 'object', 'caps.launcher is object');
    assert(typeof caps.catalog === 'object', 'caps.catalog is object');

    // 2. Catalog listing & searching
    const apps = bro.apps.list();
    assert(Array.isArray(apps), 'bro.apps.list() returns an array');

    const searchResults = bro.apps.search('text');
    assert(Array.isArray(searchResults), 'bro.apps.search() returns an array');

    // 3. MIME querying
    const defaultApp = bro.apps.getDefaultApp('text/plain');
    assert(defaultApp === null || typeof defaultApp === 'object', 'getDefaultApp returns null or object');

    const mimeApps = bro.apps.getAppsForMime('text/plain');
    assert(Array.isArray(mimeApps), 'getAppsForMime returns an array');

    // 4. Icon resolution
    const icon = bro.apps.resolveIcon('text-editor', { size: 48 });
    assert(icon === null || typeof icon === 'string', 'resolveIcon returns null or path string');

    // 5. Recent items: read only. addRecent/clearRecent write the account's
    // real recent-files list (the shell's Recent folder on Windows,
    // recently-used.xbel on Linux), so broapps' own tests cover them against
    // an isolated store, never this one.
    const recentItems = bro.apps.getRecent(5);
    assert(Array.isArray(recentItems), 'getRecent returns an array');

    // 6. Watcher
    const watcher = bro.apps.watch(() => {});
    assert(typeof watcher === 'object', 'watcher is an object');
    assert(typeof watcher.unwatch === 'function', 'watcher has unwatch()');
    watcher.unwatch();

    // 7. Refresh
    bro.apps.refresh();

    console.log('test_apps.js PASSED');
}
