// bro.math.SpatialHash3D — uniform-grid 3D spatial index over points and
// spheres. Covers construction, insert/insertSphere, radiusQuery, queryAABB,
// remove, nearest, and the size/cellSize/maxRadius accessors.

// Both spellings are documented as equivalent.
assert(bro.math.SpatialHash3D === SpatialHash3D, 'SpatialHash3D aliased onto bro.math');

// ---- construction / accessors ---------------------------------------------

const h0 = new SpatialHash3D(2.5);
assert(h0.cellSize === 2.5, 'cellSize reflects constructor arg; got ' + h0.cellSize);
assert(h0.size === 0, 'fresh hash is empty');
assert(h0.maxRadius === 0, 'fresh hash has zero maxRadius');

const hDefault = new SpatialHash3D();
assert(hDefault.cellSize === 1, 'default cellSize is 1; got ' + hDefault.cellSize);

// ---- empty-hash query -------------------------------------------------------

assert(Array.isArray(h0.radiusQuery(0, 0, 0, 5)), 'radiusQuery returns an array even when empty');
assert(h0.radiusQuery(0, 0, 0, 5).length === 0, 'radiusQuery on empty hash returns no ids');
assert(h0.queryAABB(-5, -5, -5, 5, 5, 5).length === 0, 'queryAABB on empty hash returns no ids');
assert(h0.nearest(0, 0, 0, 5) === -1, 'nearest on empty hash returns -1');

// ---- single-point hash -------------------------------------------------------

const single = new SpatialHash3D(1.0);
single.insert(1, 2, 3, 7);
assert(single.size === 1, 'size after one insert');
assert(single.radiusQuery(1, 2, 3, 0.001).length === 1, 'exact-center radius query hits the point');
assert(single.radiusQuery(1, 2, 3, 0.001)[0] === 7, 'returned id matches inserted id');
assert(single.radiusQuery(100, 100, 100, 1).length === 0, 'far-away radius query misses');
assert(single.nearest(1, 2, 3.5, 5) === 7, 'nearest finds the only point');

// ---- insert: hand-checkable point layout -----------------------------------
// A small deterministic grid of points on the X axis: ids 0..5 at x = 0,1,2,3,4,5.

const h = new SpatialHash3D(1.0);
for (let i = 0; i <= 5; i++) h.insert(i, 0, 0, i);
assert(h.size === 6, 'six points inserted; got ' + h.size);

{
    // radius 1.5 around x=2 should reach ids at x=1,2,3 (dist 1,0,1) but not
    // x=0 (dist 2) or x=4 (dist 2).
    const ids = h.radiusQuery(2, 0, 0, 1.5).slice().sort();
    assert(ids.length === 3, 'radiusQuery(2,0,0,1.5) returns 3 ids; got ' + JSON.stringify(ids));
    assert(ids[0] === 1 && ids[1] === 2 && ids[2] === 3, 'radiusQuery returns ids 1,2,3; got ' + JSON.stringify(ids));
}

{
    // Box query [0.5, 3.5] on X should catch ids 1,2,3 only.
    const ids = h.queryAABB(0.5, -0.5, -0.5, 3.5, 0.5, 0.5).slice().sort();
    assert(ids.length === 3, 'queryAABB box returns 3 ids; got ' + JSON.stringify(ids));
    assert(ids[0] === 1 && ids[1] === 2 && ids[2] === 3, 'queryAABB returns ids 1,2,3; got ' + JSON.stringify(ids));
}

{
    // nearest to x=2.4 among integer points 0..5 should be id 2.
    const id = h.nearest(2.4, 0, 0, 10);
    assert(id === 2, 'nearest(2.4,0,0) picks id 2; got ' + id);
}

// ---- remove + re-query -------------------------------------------------------

h.remove(2);
assert(h.size === 5, 'size drops after remove; got ' + h.size);
{
    const ids = h.radiusQuery(2, 0, 0, 1.5).slice().sort();
    assert(ids.indexOf(2) === -1, 'removed id no longer returned');
    assert(ids.length === 2 && ids[0] === 1 && ids[1] === 3, 'remaining neighbours still returned; got ' + JSON.stringify(ids));
}

// clear() empties everything but keeps cellSize.
h.clear();
assert(h.size === 0, 'size after clear');
assert(h.cellSize === 1, 'cellSize survives clear');
assert(h.radiusQuery(2, 0, 0, 100).length === 0, 'no ids left after clear');

// reset() re-clears AND changes cell size.
h.insert(0, 0, 0, 99);
h.reset(4);
assert(h.size === 0, 'reset clears entries');
assert(h.cellSize === 4, 'reset changes cellSize; got ' + h.cellSize);

// ---- insertSphere: distant center whose surface reaches the query ----------
// Mirrors the native bromath coverage: a big sphere centered far from the
// query point still matches because its surface reaches; a small one at the
// same center does not.

const sh = new SpatialHash3D(0.5);
sh.insertSphere(5, 0, 0, 2.0, 100);  // surface at x=3, does not reach origin
sh.insertSphere(5, 0, 0, 10.0, 101); // surface at x=-5, reaches past origin
sh.insertSphere(50, 0, 0, 1.0, 102); // far away, should always miss

assert(sh.maxRadius === 10, 'maxRadius tracks the largest sphere inserted; got ' + sh.maxRadius);

{
    const ids = new Set(sh.radiusQuery(0, 0, 0, 0.5));
    assert(ids.has(101), 'huge sphere reaches the origin query');
    assert(!ids.has(100), 'small sphere at x=5 does not reach the origin');
    assert(!ids.has(102), 'far sphere does not reach the origin');
}

// Mixed point + sphere inserts in the same grid.
const mixed = new SpatialHash3D(1.0);
mixed.insert(0, 0, 0, 10);
mixed.insertSphere(3, 0, 0, 2.5, 20); // surface at x=0.5, reaches close to origin
mixed.insert(5, 0, 0, 30);

{
    const ids = new Set(mixed.radiusQuery(0, 0, 0, 0.6));
    assert(ids.has(10), 'point at origin matches its own query');
    assert(ids.has(20), 'sphere reaching near the origin matches');
    assert(!ids.has(30), 'distant point does not match');
}

// ---- queryAABB with spheres --------------------------------------------------

const shBox = new SpatialHash3D(0.5);
shBox.insertSphere(2.5, 0.5, 0.5, 2.0, 1); // reaches x=0.5, inside the box
shBox.insertSphere(2.5, 0.5, 0.5, 0.5, 2); // reaches x=2.0, stays outside the box
shBox.insert(0.5, 0.5, 0.5, 3);            // fully inside the box

{
    const ids = new Set(shBox.queryAABB(0, 0, 0, 1, 1, 1));
    assert(ids.has(1), 'big sphere intrudes into the box');
    assert(!ids.has(2), 'small sphere does not reach the box');
    assert(ids.has(3), 'point fully inside the box is returned');
}

// ---- chaining: methods documented as "returns this" ------------------------

const chain = new SpatialHash3D(1);
assert(chain.insert(0, 0, 0, 1) === chain, 'insert returns this');
assert(chain.insertSphere(0, 0, 0, 1, 2) === chain, 'insertSphere returns this');
assert(chain.remove(1) === chain, 'remove returns this');
assert(chain.clear() === chain, 'clear returns this');
assert(chain.reset(1) === chain, 'reset returns this');

// ---- ids: int32, anything else a RangeError ------------------------------------

{
    const ids = new SpatialHash3D(1);
    const throwsRange = (fn) => {
        try { fn(); } catch (e) { return e instanceof RangeError; }
        return false;
    };
    for (const bad of [NaN, Infinity, -Infinity, 2 ** 31, -(2 ** 31) - 1, 1e12, undefined]) {
        assert(throwsRange(() => ids.insert(0, 0, 0, bad)), 'insert with id ' + bad + ' is a RangeError');
        assert(throwsRange(() => ids.insertSphere(0, 0, 0, 1, bad)), 'insertSphere with id ' + bad + ' is a RangeError');
        assert(throwsRange(() => ids.remove(bad)), 'remove with id ' + bad + ' is a RangeError');
    }
    assert(ids.size === 0, 'a rejected id inserts nothing, size ' + ids.size);

    // The int32 extremes are ids like any other; 2^32 + 5 no longer aliases 5.
    ids.insert(1, 0, 0, 2 ** 31 - 1);
    ids.insert(2, 0, 0, -(2 ** 31));
    ids.insert(3, 0, 0, 5);
    assert(ids.size === 3, 'the int32 extremes insert, size ' + ids.size);
    assert(throwsRange(() => ids.insert(9, 0, 0, 2 ** 32 + 5)), '2^32 + 5 is out of range');
    assert(ids.nearest(3, 0, 0, 0.5) === 5, 'id 5 kept its position');
    assert(ids.nearest(1, 0, 0, 0.5) === 2 ** 31 - 1, 'the largest id round-trips');
    ids.remove(-(2 ** 31));
    assert(ids.size === 2, 'the smallest id removes, size ' + ids.size);
}
