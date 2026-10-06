// Headless test for bro.apps
assert(typeof bro.apps === 'object', 'bro.apps namespace exists');
assert(bro.apps.available === true, 'bro.apps.available is true');

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

// 5. Recent items
assert(bro.apps.addRecent('/tmp/test_recent_item.txt', 'test.desktop') === true, 'addRecent returns true');
const recentItems = bro.apps.getRecent(5);
assert(Array.isArray(recentItems), 'getRecent returns an array');
assert(recentItems.some(i => i.filePath === '/tmp/test_recent_item.txt'), 'recent item was recorded');

// 6. Watcher
let watchCalled = false;
const watcher = bro.apps.watch((event) => {
    watchCalled = true;
});
assert(typeof watcher === 'object', 'watcher is an object');
assert(typeof watcher.unwatch === 'function', 'watcher has unwatch()');
watcher.unwatch();

// 7. Refresh
bro.apps.refresh();

console.log('test_apps.js PASSED');
