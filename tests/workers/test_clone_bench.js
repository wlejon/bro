// The structured clone a Worker postMessage crosses as (host_worker_msg.cpp),
// timed on the shapes a game ships between threads: a Map of 10k small wall
// objects (what took ~500 ms to deserialize on the main thread), an array of
// records with nested arrays, and typed arrays copied and transferred.
//
// __host.cloneBench(value, transfer?) runs both halves in this realm and times
// them apart. Correctness is asserted in full; the timings are printed (the
// numbers before/after an optimization are read off this output), and the
// only time bound is a loose ceiling that catches a return to the old cost.
//
// Best-of-reps on the dev box (Release, ~15% background CPU), before the
// layout/zero-copy rewrite -> after:
//   Map<string,wall> x10000   serialize 39.12 -> 1.71 ms   deserialize  9.76 -> 3.54 ms
//   Array<record> x10000      serialize 85.81 -> 3.05 ms   deserialize 16.10 -> 4.60 ms
//   Float32Array 4M copy      serialize  1.94 -> 2.06 ms   deserialize  4.24 -> 0.01 ms
//   Float32Array 4M transfer  serialize  1.96 -> 2.11 ms   deserialize  4.24 -> 0.00 ms
// (a buffer past 4 KB crosses out of band and the receiver adopts the block.)

function wallsMap(n) {
    const m = new Map();
    for (let i = 0; i < n; i++) {
        const x = i % 200, y = (i / 200) | 0, dir = i & 3;
        m.set(x + ',' + y + ',' + dir, { x, y, dir, type: (i * 7) % 5, hp: 100 - (i % 13), breached: (i % 11) === 0 });
    }
    return m;
}

function records(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        out.push({ id: i, name: 'unit-' + i, pos: [i * 0.5, i * 0.25, -i], tags: ['a', 'b'], score: i / 3 });
    }
    return out;
}

function best(label, make, reps, check, transfer) {
    let s = Infinity, d = Infinity, bytes = 0;
    for (let r = 0; r < reps; r++) {
        const v = make();
        const res = transfer ? __host.cloneBench(v, transfer(v)) : __host.cloneBench(v);
        s = Math.min(s, res.serializeMs);
        d = Math.min(d, res.deserializeMs);
        bytes = res.bytes;
        if (r === 0) check(res.value, v);
    }
    console.log('[clone-bench] ' + label.padEnd(28) + ' serialize ' + s.toFixed(2).padStart(8) + ' ms   deserialize ' +
                d.toFixed(2).padStart(8) + ' ms   ' + bytes + ' bytes');
    return { s, d };
}

// ---- Map of 10k wall objects -------------------------------------------------
const walls = best('Map<string,wall> x10000', () => wallsMap(10000), 5, (got) => {
    assert(got instanceof Map, 'a Map arrives as a Map');
    assert(got.size === 10000, 'all 10000 entries, got ' + got.size);
    const w = got.get('13,2,1');
    const i = 2 * 200 + 13;
    assert(w && w.x === 13 && w.y === 2 && w.dir === 1 && w.type === (i * 7) % 5 && w.hp === 100 - (i % 13),
           'an entry keeps its fields: ' + JSON.stringify(w));
    let n = 0;
    for (const [k, v] of got) { if (n++ === 0) assert(k === '0,0,0' && v.x === 0, 'insertion order kept'); }
});

// ---- array of records --------------------------------------------------------
best('Array<record> x10000', () => records(10000), 5, (got) => {
    assert(Array.isArray(got) && got.length === 10000, 'array length kept');
    const r = got[777];
    assert(r.id === 777 && r.name === 'unit-777' && r.pos[1] === 777 * 0.25 && r.tags[1] === 'b' && r.score === 777 / 3,
           'a record keeps its fields: ' + JSON.stringify(r));
});

// ---- shared sub-objects and a cycle still resolve ------------------------------
{
    const shared = { k: 1 };
    const root = { a: shared, b: shared, list: [shared] };
    root.self = root;
    const got = __host.cloneBench(root).value;
    assert(got.a === got.b && got.list[0] === got.a, 'a shared object arrives once');
    assert(got.self === got, 'a cycle closes on the clone');
}

// ---- typed arrays: copied, and transferred ------------------------------------
best('Float32Array 4M copy', () => new Float32Array(4 << 20).fill(1.5), 3, (got) => {
    assert(got instanceof Float32Array && got.length === 4 << 20 && got[12345] === 1.5, 'a copied typed array arrives whole');
});
best('Float32Array 4M transfer', () => new Float32Array(4 << 20).fill(2.5), 3, (got, sent) => {
    assert(got instanceof Float32Array && got.length === 4 << 20 && got[4000000] === 2.5, 'a transferred typed array arrives whole');
    assert(sent.length === 0 && sent.buffer.byteLength === 0, 'the sender\'s buffer is detached');
}, (v) => [v.buffer]);

{
    const buf = new ArrayBuffer(64);
    const a = new Uint8Array(buf, 8, 16), b = new Uint32Array(buf, 32, 4);
    a[0] = 9; b[3] = 0xdeadbeef;
    const got = __host.cloneBench({ a, b }).value;
    assert(got.a.buffer === got.b.buffer, 'two views keep one buffer');
    assert(got.a.byteOffset === 8 && got.b.byteOffset === 32 && got.a[0] === 9 && got.b[3] === 0xdeadbeef, 'views keep offsets and bytes');
}

// ---- the layout path's edge cases --------------------------------------------
{
    // Integer-like keys first and ascending, then insertion order.
    const got = __host.cloneBench({ b: 1, 10: 'x', a: 2, 2: 'y' }).value;
    assert(Object.keys(got).join() === '2,10,b,a', 'OwnPropertyKeys order kept: ' + Object.keys(got).join());
    // A getter runs (and its value is cloned), a non-enumerable property is dropped.
    const src = { get g() { return 7; }, plain: 1 };
    Object.defineProperty(src, 'hidden', { value: 3, enumerable: false });
    const g = __host.cloneBench(src).value;
    assert(g.g === 7 && g.plain === 1 && !('hidden' in g), 'accessor read, non-enumerable dropped: ' + JSON.stringify(g));
    // A dictionary-mode object (a delete) and UTF-16 keys and strings.
    const d = { k1: 1, k2: 2, k3: 3 };
    delete d.k2;
    d['ключ'] = 'значение ✓';
    const dg = __host.cloneBench(d).value;
    assert(Object.keys(dg).join() === 'k1,k3,ключ' && dg['ключ'] === 'значение ✓', 'dictionary + UTF-16: ' + JSON.stringify(dg));
    // Shared layouts across many objects keep each object's own values; -0 survives.
    const many = [];
    for (let i = 0; i < 100; i++) many.push({ i, s: 'n' + i, z: -0 });
    const mg = __host.cloneBench(many).value;
    assert(mg[99].i === 99 && mg[42].s === 'n42' && Object.is(mg[5].z, -0), 'layout values per object, -0 kept');
    // A Set, a Date, a class instance (clones as a plain object).
    class P { constructor() { this.x = 1; } }
    const misc = __host.cloneBench({ s: new Set([1, 'a', 1]), d: new Date(12345), p: new P() }).value;
    assert(misc.s instanceof Set && misc.s.size === 2 && misc.s.has('a'), 'Set kept');
    assert(misc.d instanceof Date && misc.d.getTime() === 12345, 'Date kept');
    assert(misc.p.x === 1 && !(misc.p instanceof P), 'class instance arrives as a plain object');
    // A small buffer stays inline; a copied large one is the receiver's own.
    const small = new Uint8Array([1, 2, 3]);
    assert(__host.cloneBench(small).value[2] === 3, 'small inline buffer');
    const big = new Uint8Array(100000).fill(9);
    const bg = __host.cloneBench(big).value;
    bg[0] = 1;
    assert(big[0] === 9 && bg[99999] === 9, 'a copied out-of-band buffer is independent of the source');
}

assert(walls.d < 150, 'deserializing the 10k-wall Map stays well under the old ~500 ms, got ' + walls.d.toFixed(1));
