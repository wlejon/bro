// Tests for apps/lib/project.js.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_project.js
//
// Uses os.tmpdir() for scratch directories; each test gets a unique path
// under there and cleans up after itself.

'use strict';

const fs   = require('fs');
const path = require('path');
const os   = require('os');

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) {
    const ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error((msg || 'eq') + ': ' + ja + ' !== ' + jb);
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }
function throws(fn, pattern, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (!caught) throw new Error((msg || 'expected throw') + ': no exception');
    if (pattern && !pattern.test(String(caught.message))) {
        throw new Error(`threw, but not matching ${pattern}: "${caught.message}"`);
    }
}

let testCounter = 0;
function freshPath() {
    testCounter++;
    return path.join(os.tmpdir(),
        `bro-project-test-${Date.now()}-${testCounter}.bro`);
}
function rmrf(p) {
    if (!fs.existsSync(p)) return;
    if (fs.statSync(p).isDirectory()) {
        for (const entry of fs.readdirSync(p)) rmrf(path.join(p, entry));
        fs.rmdirSync(p);
    } else {
        fs.unlinkSync(p);
    }
}

// --- Simple in-memory app state for round-tripping ------------------------

function makeApp(initial) {
    const state = { data: Object.assign({}, initial) };
    return {
        state,
        serialize:   () => JSON.parse(JSON.stringify(state.data)),
        deserialize: (d) => { state.data = JSON.parse(JSON.stringify(d)); },
        onNew:       () => { state.data = {}; },
    };
}

// -------------------------------------------------------------------------
// Construction
// -------------------------------------------------------------------------

t('throws without required options', () => {
    throws(() => new Project(), /requires/);
    throws(() => new Project({ app: 'x' }), /requires/);
    throws(() => new Project({ app: 'x', serialize: () => ({}) }), /requires/);
});

t('initial state: Untitled, clean, no path', () => {
    const a = makeApp({ n: 1 });
    const proj = new Project({ app: 'test', schema: 1, ...a });
    eq(proj.path, null);
    eq(proj.name, 'Untitled');
    eq(proj.isDirty(), false);
});

// -------------------------------------------------------------------------
// Save / load round-trip
// -------------------------------------------------------------------------

t('saveTo writes a bundle with project.json', () => {
    const p = freshPath();
    try {
        const a = makeApp({ hello: 'world' });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        truthy(proj.saveTo(p));
        truthy(fs.existsSync(p));
        truthy(fs.statSync(p).isDirectory());
        const projectFile = path.join(p, 'project.json');
        truthy(fs.existsSync(projectFile));
        const json = JSON.parse(fs.readFileSync(projectFile, 'utf8'));
        eq(json.app, 'test');
        eq(json.schema, 1);
        eq(json.bro_project, 1);
        truthy(json.created);
        truthy(json.modified);
        eq(json.data, { hello: 'world' });
        eq(proj.path, p);
        eq(proj.name, path.basename(p));
        eq(proj.isDirty(), false);
    } finally { rmrf(p); }
});

t('openPath round-trips state', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 42, items: ['a', 'b', 'c'] });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);

        const b = makeApp({});   // fresh app
        const proj2 = new Project({ app: 'test', schema: 1, ...b });
        truthy(proj2.openPath(p));
        eq(b.state.data, { n: 42, items: ['a', 'b', 'c'] });
        eq(proj2.path, p);
        eq(proj2.isDirty(), false);
    } finally { rmrf(p); }
});

t('save without path returns false', () => {
    const a = makeApp({ n: 1 });
    const proj = new Project({ app: 'test', schema: 1, ...a });
    eq(proj.save(), false);
});

t('save after saveTo writes to the same path', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        a.state.data.n = 2;
        proj.markDirty();
        eq(proj.save(), true);
        const json = JSON.parse(fs.readFileSync(path.join(p, 'project.json'), 'utf8'));
        eq(json.data.n, 2);
    } finally { rmrf(p); }
});

t('created timestamp preserved across saves', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        const j1 = JSON.parse(fs.readFileSync(path.join(p, 'project.json'), 'utf8'));
        // Wait a tick so modified timestamp can diverge.
        for (let i = 0; i < 5; i++) proj.save();
        const j2 = JSON.parse(fs.readFileSync(path.join(p, 'project.json'), 'utf8'));
        eq(j1.created, j2.created, 'created unchanged');
    } finally { rmrf(p); }
});

t('open refuses wrong app id', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        const b = makeApp({});
        const proj2 = new Project({ app: 'other', schema: 1, ...b });
        throws(() => proj2.openPath(p), /expected app "other"/);
    } finally { rmrf(p); }
});

t('openPath rejects missing directory', () => {
    const missing = path.join(os.tmpdir(), 'bro-project-nope-' + Date.now() + '.bro');
    const a = makeApp({});
    const proj = new Project({ app: 'test', schema: 1, ...a });
    throws(() => proj.openPath(missing), /no such path/);
});

t('openPath rejects directory without project.json', () => {
    const p = freshPath();
    try {
        fs.mkdirSync(p, { recursive: true });
        const a = makeApp({});
        const proj = new Project({ app: 'test', schema: 1, ...a });
        throws(() => proj.openPath(p), /no project\.json/);
    } finally { rmrf(p); }
});

// -------------------------------------------------------------------------
// Dirty tracking
// -------------------------------------------------------------------------

t('markDirty / markClean', () => {
    const a = makeApp({ n: 1 });
    const proj = new Project({ app: 'test', schema: 1, ...a });
    const events = [];
    proj.on('dirty', (p) => events.push(p.dirty));
    proj.markDirty();
    eq(proj.isDirty(), true);
    proj.markDirty();           // idempotent
    proj.markClean();
    eq(proj.isDirty(), false);
    proj.markClean();           // idempotent
    eq(events, [true, false]);
});

t('save clears dirty', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.markDirty();
        proj.saveTo(p);
        eq(proj.isDirty(), false);
    } finally { rmrf(p); }
});

t('history integration: record → dirty', () => {
    const a = makeApp({ n: 1 });
    const h = new History();
    const proj = new Project({ app: 'test', schema: 1, ...a, history: h });
    eq(proj.isDirty(), false);
    h.do('bump', () => { a.state.data.n = 2; }, () => { a.state.data.n = 1; });
    eq(proj.isDirty(), true);
});

t('history integration: load clears history + stays clean', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const h = new History();
        const proj = new Project({ app: 'test', schema: 1, ...a, history: h });
        h.do('bump', () => { a.state.data.n = 2; }, () => { a.state.data.n = 1; });
        truthy(proj.isDirty());
        proj.saveTo(p);
        eq(proj.isDirty(), false);

        // Add more history, then reload — the open must clear history + dirty.
        h.do('bump2', () => { a.state.data.n = 3; }, () => { a.state.data.n = 2; });
        truthy(proj.isDirty());
        proj.openPath(p);
        eq(proj.isDirty(), false);
        eq(h.size(), 0, 'history cleared by load');
        eq(a.state.data.n, 2, 'data reverted to saved value');
    } finally { rmrf(p); }
});

// -------------------------------------------------------------------------
// Migrations
// -------------------------------------------------------------------------

t('loads current-schema file without migration', () => {
    const p = freshPath();
    try {
        fs.mkdirSync(p, { recursive: true });
        fs.writeFileSync(path.join(p, 'project.json'), JSON.stringify({
            bro_project: 1, app: 'test', schema: 2,
            data: { hello: 'v2' },
        }), 'utf8');
        const a = makeApp({});
        const proj = new Project({ app: 'test', schema: 2, ...a });
        proj.openPath(p);
        eq(a.state.data, { hello: 'v2' });
    } finally { rmrf(p); }
});

t('migrations run sequentially', () => {
    const p = freshPath();
    try {
        fs.mkdirSync(p, { recursive: true });
        fs.writeFileSync(path.join(p, 'project.json'), JSON.stringify({
            bro_project: 1, app: 'test', schema: 1,
            data: { val: 'v1' },
        }), 'utf8');
        const a = makeApp({});
        const proj = new Project({
            app: 'test', schema: 3, ...a,
            migrations: {
                1: (d) => ({ ...d, v2added: true }),
                2: (d) => ({ ...d, v3added: true }),
            },
        });
        proj.openPath(p);
        eq(a.state.data, { val: 'v1', v2added: true, v3added: true });
    } finally { rmrf(p); }
});

t('throws on missing migration', () => {
    const p = freshPath();
    try {
        fs.mkdirSync(p, { recursive: true });
        fs.writeFileSync(path.join(p, 'project.json'), JSON.stringify({
            bro_project: 1, app: 'test', schema: 1, data: {},
        }), 'utf8');
        const a = makeApp({});
        const proj = new Project({ app: 'test', schema: 3, ...a });
        throws(() => proj.openPath(p), /no migration from schema 1/);
    } finally { rmrf(p); }
});

t('throws on file newer than app', () => {
    const p = freshPath();
    try {
        fs.mkdirSync(p, { recursive: true });
        fs.writeFileSync(path.join(p, 'project.json'), JSON.stringify({
            bro_project: 1, app: 'test', schema: 5, data: {},
        }), 'utf8');
        const a = makeApp({});
        const proj = new Project({ app: 'test', schema: 2, ...a });
        throws(() => proj.openPath(p), /newer than app/);
    } finally { rmrf(p); }
});

// -------------------------------------------------------------------------
// new()
// -------------------------------------------------------------------------

t('new() calls onNew, clears path + dirty', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 99 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        proj.markDirty();
        eq(proj.new(), true);
        eq(a.state.data, {}, 'onNew reset state');
        eq(proj.path, null);
        eq(proj.isDirty(), false);
    } finally { rmrf(p); }
});

t('new() respects promptDirty when dirty', () => {
    const a = makeApp({ n: 1 });
    const proj = new Project({
        app: 'test', schema: 1, ...a,
        promptDirty: () => false,
    });
    proj.markDirty();
    eq(proj.new(), false);
    eq(a.state.data, { n: 1 }, 'state untouched when prompt denied');
});

t('new() bypasses prompt when clean', () => {
    let promptCalls = 0;
    const a = makeApp({ n: 1 });
    const proj = new Project({
        app: 'test', schema: 1, ...a,
        promptDirty: () => { promptCalls++; return true; },
    });
    // clean → no prompt expected
    eq(proj.new(), true);
    eq(promptCalls, 0);
});

// -------------------------------------------------------------------------
// Events
// -------------------------------------------------------------------------

t('events fire on save / load / new', () => {
    const p = freshPath();
    try {
        const a = makeApp({ n: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        const seen = [];
        proj.on('saved',  (p) => seen.push(['saved',  !!p.path]));
        proj.on('loaded', (p) => seen.push(['loaded', !!p.path]));
        proj.on('new',    ()  => seen.push(['new']));
        proj.saveTo(p);
        proj.new();
        proj.openPath(p);
        eq(seen, [['saved', true], ['new'], ['loaded', true]]);
    } finally { rmrf(p); }
});

// -------------------------------------------------------------------------
// saveTo path handling
// -------------------------------------------------------------------------

t('saveTo creates nested parent directories', () => {
    const p = path.join(os.tmpdir(), 'bro-proj-nested-' + Date.now(),
                        'deep', 'down', 'proj.bro');
    try {
        const a = makeApp({ v: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        truthy(fs.existsSync(path.join(p, 'project.json')));
    } finally {
        rmrf(path.join(os.tmpdir(), 'bro-proj-nested-' + (p.match(/\d+/) || [0])[0]));
    }
});

t('saveTo overwrites existing project.json atomically', () => {
    const p = freshPath();
    try {
        const a = makeApp({ v: 1 });
        const proj = new Project({ app: 'test', schema: 1, ...a });
        proj.saveTo(p);
        a.state.data.v = 2;
        proj.saveTo(p);
        const json = JSON.parse(fs.readFileSync(path.join(p, 'project.json'), 'utf8'));
        eq(json.data.v, 2);
        // The tmp file must not be left behind.
        falsy(fs.existsSync(path.join(p, 'project.json.tmp')));
    } finally { rmrf(p); }
});

// -------------------------------------------------------------------------
// End
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
