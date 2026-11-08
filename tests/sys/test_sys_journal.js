// Headless test for bro.sys.journal: the real journal, read-only but for one
// record it writes itself through logger(1) to see a live follow deliver it.
// Skips where there is no journal (not Linux, no journald, nothing readable).
const j = bro.sys && bro.sys.journal;
assert(typeof j === 'object' && j !== null, 'bro.sys.journal exists');

async function main() {
    if (j.available === false) return skipTest('bro.sys.journal is unavailable: ' + j.reason);

    let page;
    try {
        page = await j.query({ limit: 20 });
    } catch (e) {
        return skipTest('no readable journal: ' + e.message);
    }
    if (page.entries.length === 0) return skipTest('the journal is empty');

    // Shapes, oldest first.
    assert(Array.isArray(page.entries) && page.entries.length <= 20, 'a page of at most limit entries');
    assert(typeof page.more === 'boolean', 'more is a boolean');
    for (let i = 0; i < page.entries.length; i++) {
        const e = page.entries[i];
        assert(typeof e.cursor === 'string' && e.cursor.length > 0, 'entry has a cursor');
        assert(typeof e.time === 'number' && e.time > 1e12, 'entry time is ms since the epoch');
        assert(typeof e.message === 'string', 'entry message is a string');
        assert(e.priority === null || (e.priority >= 0 && e.priority <= 7), 'priority 0..7 or null');
        assert(typeof e.bootId === 'string' && e.bootId.length === 32, 'bootId is 32 hex digits');
        assert(e.fields === undefined, 'no fields without allFields');
        if (i) assert(e.time >= page.entries[i - 1].time, 'entries are oldest first');
    }

    // The page before it: older, no overlap.
    if (page.more) {
        const older = await j.query({ limit: 20, before: page.before });
        assert(older.entries.length > 0, 'the older page has entries');
        const seen = new Set(page.entries.map(e => e.cursor));
        assert(older.entries.every(e => !seen.has(e.cursor)), 'pages do not overlap');
        assert(older.entries.at(-1).time <= page.entries[0].time, 'the older page is older');
    }

    // Priority filter, by name.
    const errs = await j.query({ limit: 50, priority: 'err' });
    assert(errs.entries.every(e => e.priority !== null && e.priority <= 3), 'priority err keeps 0..3');

    // A bad option rejects with an Error.
    let rejected = null;
    try { await j.query({ priority: 'loud' }); } catch (e) { rejected = e; }
    assert(rejected instanceof Error && /priority/.test(rejected.message), 'an unknown priority rejects: ' + rejected);

    // Boots: the newest is offset 0 and is this boot.
    const boots = await j.boots();
    assert(Array.isArray(boots) && boots.length > 0, 'boots lists at least this boot');
    assert(boots.at(-1).offset === 0, 'the newest boot is offset 0');
    const thisBoot = await j.query({ boot: 'current', limit: 1, allFields: true });
    if (thisBoot.entries.length) {
        const e = thisBoot.entries[0];
        assert(e.bootId === boots.at(-1).id, 'boot current is the newest boot');
        assert(typeof e.fields === 'object' && e.fields.MESSAGE === e.message, 'allFields carries every field');
    }

    const transports = await j.values('_TRANSPORT');
    assert(Array.isArray(transports) && transports.length > 0, 'values lists a field\'s values');

    // A live follow from the newest record: a record written now arrives,
    // under an identifier of this run's own, so the match has no record
    // before it. Following after a cursor (not "new records only") means the
    // record is delivered however late the follow's thread starts.
    const { execFileSync } = require('child_process');
    const tag = 'bro-journal-test-' + Math.random().toString(16).slice(2);
    const got = [];
    const head = (await j.query({ limit: 1 })).entries.at(-1);
    const follow = j.follow({ match: { SYSLOG_IDENTIFIER: tag }, after: head ? head.cursor : undefined }, (entries) => {
        for (const e of entries) got.push(e);
    });
    assert(typeof follow.id === 'number' && typeof follow.close === 'function', 'follow returns { id, close }');
    try {
        execFileSync('logger', ['-t', tag, tag]);
    } catch (e) {
        follow.close();
        return skipTest('logger(1) is not available to write a record: ' + e.message);
    }
    for (let i = 0; i < 100 && !got.some(e => e.message === tag); i++) await new Promise(r => setTimeout(r, 50));
    follow.close();
    assert(got.some(e => e.message === tag && e.identifier === tag),'the follow delivered the new record');

    // And the query sees it, by text.
    const found = await j.query({ text: tag.toUpperCase(), since: Date.now() - 60000 });
    assert(found.entries.length === 1 && found.entries[0].message === tag, 'text search finds the record');
}

await main();
