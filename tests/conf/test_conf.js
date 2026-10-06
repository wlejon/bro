// Test bro.conf API — schemas, typed getters, Promise-based setters, defaults, and watchers.
// Backed by native broconf (BRO_WITH_CONF).

assert(typeof bro === 'object', 'bro root object exists');
assert(typeof bro.conf === 'object', 'bro.conf namespace exists');
assert(bro.conf.available === true, 'bro.conf.available is true');

// =========================================================================
// 1. Schema Registration
// =========================================================================
bro.conf.registerSchema({
    id: 'org.bro.headless.test',
    path: 'headless.conf',
    keys: {
        enabled: { type: 'bool', default: true },
        count: { type: 'int', default: 5, min: 1, max: 50 },
        ratio: { type: 'double', default: 1.5, min: 0.0, max: 10.0 },
        title: { type: 'string', default: 'Default Title' },
        items: { type: 'string_list', default: ['one', 'two'] },
        mode: { type: 'enum', default: 'fast', enum: ['slow', 'fast', 'auto'] },
        color: { type: 'color', default: { r: 10, g: 20, b: 30, a: 255 } },
        rect: { type: 'rect', default: { x: 0, y: 0, width: 640, height: 480 } },
        details: { type: 'dictionary', default: { key1: 'val1', sub: 42 } }
    }
});

// =========================================================================
// 2. Introspection (has, isDefault, listKeys)
// =========================================================================
assert(bro.conf.has('headless.conf.enabled'), 'has dotted key');
assert(bro.conf.has('headless.conf', 'count'), 'has path, key');
assert(!bro.conf.has('headless.conf.nonexistent'), 'nonexistent key not found');
assert(bro.conf.isDefault('headless.conf.enabled'), 'enabled is default');
assert(bro.conf.isDefault('headless.conf', 'count'), 'count is default');

const keys = bro.conf.listKeys('headless.conf');
assert(Array.isArray(keys), 'listKeys returns an array');
assert(keys.length === 9, 'listKeys returns all 9 keys, got ' + keys.length);
assert(keys.includes('enabled') && keys.includes('count') && keys.includes('mode'), 'keys list contains registered items');

// =========================================================================
// 3. Defaults via get and getOptional
// =========================================================================
assert(bro.conf.get('headless.conf.enabled') === true, 'bool default');
assert(bro.conf.get('headless.conf', 'count') === 5, 'int default');
assert(Math.abs(bro.conf.get('headless.conf.ratio') - 1.5) < 0.001, 'double default');
assert(bro.conf.get('headless.conf.title') === 'Default Title', 'string default');

const items = bro.conf.get('headless.conf.items');
assert(Array.isArray(items) && items.length === 2 && items[0] === 'one' && items[1] === 'two', 'string_list default');

assert(bro.conf.get('headless.conf.mode') === 'fast', 'enum default string');

const color = bro.conf.get('headless.conf.color');
assert(typeof color === 'object' && color.r === 10 && color.g === 20 && color.b === 30 && color.a === 255, 'color default');

const rect = bro.conf.get('headless.conf.rect');
assert(typeof rect === 'object' && rect.x === 0 && rect.y === 0 && rect.width === 640 && rect.height === 480, 'rect default');

const details = bro.conf.get('headless.conf.details');
assert(typeof details === 'object' && details.key1 === 'val1' && details.sub === 42, 'dictionary default');

assert(bro.conf.getOptional('headless.conf.missing') === undefined, 'getOptional missing returns undefined');

let getThrew = false;
try {
    bro.conf.get('headless.conf.missing');
} catch (e) {
    getThrew = true;
}
assert(getThrew, 'get missing key throws');

// =========================================================================
// 4. Setting values and Promise resolution
// =========================================================================
await (async function() {
    await bro.conf.set('headless.conf.count', 25);
    assert(bro.conf.get('headless.conf.count') === 25, 'count set to 25');
    assert(!bro.conf.isDefault('headless.conf.count'), 'count is no longer default');

    await bro.conf.set('headless.conf', 'title', 'Custom Title');
    assert(bro.conf.get('headless.conf.title') === 'Custom Title', 'title set via (path, key)');

    // Validation rejection on out-of-range
    let rejectRange = false;
    try {
        await bro.conf.set('headless.conf.count', 999);
    } catch (e) {
        rejectRange = true;
    }
    assert(rejectRange, 'setting count out of range (max 50) rejects Promise');

    // Validation rejection on invalid enum
    let rejectEnum = false;
    try {
        await bro.conf.set('headless.conf.mode', 'turbo');
    } catch (e) {
        rejectEnum = true;
    }
    assert(rejectEnum, 'setting invalid enum rejects Promise');
})();

// =========================================================================
// 5. Resetting to default
// =========================================================================
assert(bro.conf.reset('headless.conf.count') === true, 'reset count returns true');
assert(bro.conf.isDefault('headless.conf.count'), 'count is default after reset');
assert(bro.conf.get('headless.conf.count') === 5, 'count value back to schema default 5');

// =========================================================================
// 6. Watchers and frame pump delivery
// =========================================================================
await (async function() {
    const events = [];
    const handle = bro.conf.watch('headless.conf', (key, newVal, oldVal) => {
        events.push({ key, newVal, oldVal });
    });
    assert(handle && typeof handle.token === 'number', 'watch returns handle with token');

    await bro.conf.set('headless.conf.title', 'Watched Title');

    // Changes queue across threads and wait for the frame pump
    assert(events.length === 0, 'events array empty before advanceTime');
    advanceTime(16);
    assert(events.length === 1, 'event delivered after advanceTime');
    assert(events[0].key === 'title', 'event key matches title');
    assert(events[0].newVal === 'Watched Title', 'event newVal matches');
    assert(events[0].oldVal === 'Custom Title', 'event oldVal matches');

    // Unwatch using handle method
    handle.unwatch();

    await bro.conf.set('headless.conf.title', 'Post Unwatch Title');
    advanceTime(16);
    assert(events.length === 1, 'no further events after unwatch');

    // Key-specific watch
    const keyEvents = [];
    const h2 = bro.conf.watch('headless.conf', 'enabled', (key, newVal, oldVal) => {
        keyEvents.push({ key, newVal, oldVal });
    });
    await bro.conf.set('headless.conf.enabled', false);
    advanceTime(16);
    assert(keyEvents.length === 1 && keyEvents[0].newVal === false, 'key-specific watch fired');

    // Unwatch using bro.conf.unwatch(token)
    assert(bro.conf.unwatch(h2.token) === true, 'unwatch by token returns true');
})();

console.log('test_conf.js PASSED');
