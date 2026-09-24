// NavGrid.field(): the square-grid integration / flow field, and the cell
// costs setCellCost stores and findPath / field charge.

const nav = bro.ai.game.createNavGrid({ minX: 0, maxX: 40, minZ: 0, maxZ: 20, cellSize: 1 });
const W = nav.width, H = nav.height;
const at = (x, z) => z * W + x;

// Open ground: straight-line costs, flow toward the goal.
{
    const f = nav.field(30.5, 10.5);
    assert(f.width === 40 && f.height === 20 && f.reached === 800, 'every cell reached: ' + f.reached);
    assert(f.dist instanceof Float32Array && f.flowX instanceof Float32Array && f.flowZ instanceof Float32Array,
           'typed arrays');
    assert(f.dist[at(30, 10)] === 0, 'goal is zero');
    const d = f.dist[at(10, 0)], e = Math.hypot(20, 10);
    assert(d > e * 0.97 && d < e * 1.08, 'eikonal ~ euclidean: ' + d + ' vs ' + e);
    assert(f.flowX[at(10, 10)] > 0.99, 'west of the goal flows east');
    assert(f.flowZ[at(30, 2)] > 0.99, 'north of the goal flows +z');
    const p = nav.field({ x: 30.5, z: 10.5 });
    assert(p.dist[at(10, 0)] === d, 'point form gives the same field');
}

// A wall with one gap: detour, blocked cells Infinity, flow bends to the gap.
for (let z = 0; z < 20; z++) if (z !== 15) nav.setWalkable(20.5, z + 0.5, false);
{
    const f = nav.field(35.5, 5.5);
    assert(f.dist[at(20, 5)] === Infinity, 'wall cell is Infinity');
    assert(f.dist[at(5, 5)] > 34 && f.dist[at(5, 5)] < 39, 'route via the gap: ' + f.dist[at(5, 5)]);
    assert(f.flowZ[at(18, 5)] > 0.5, 'flow near the wall turns toward the gap');
}
nav.setWalkable(20.5, 15.5, false);
assert(nav.field(35.5, 5.5).dist[at(5, 5)] === Infinity, 'walled off: unreached');
for (let z = 0; z < 20; z++) nav.setWalkable(20.5, z + 0.5, true);

// Costs: stored (setCellCost), added (extraCost), replaced (costs).
{
    const open = nav.field(35.5, 10.5).dist[at(5, 10)];
    for (let z = 0; z < 20; z++) for (let x = 18; x < 22; x++) nav.setCellCost(x + 0.5, z + 0.5, 5);
    assert(nav.cellCost(19.5, 3.5) === 5 && nav.cellCost(1.5, 3.5) === 1, 'cellCost reads back');
    const muddy = nav.field(35.5, 10.5).dist[at(5, 10)];
    assert(Math.abs(muddy - (open + 16)) < 0.5, 'a mud band costs its price: ' + open + ' -> ' + muddy);
    const danger = new Float32Array(W * H);
    danger[at(10, 10)] = 100;
    const e = nav.field(35.5, 10.5, { extraCost: danger }).dist[at(5, 10)];
    assert(e > muddy && e < muddy + 5, 'extraCost is walked round: ' + e);
    const flat = new Uint8Array(W * H).fill(1);
    const o = nav.field(35.5, 10.5, { costs: flat }).dist[at(5, 10)];
    assert(Math.abs(o - open) < 1e-3, 'costs replaces the stored costs');
    let threw = false;
    try { nav.field(35.5, 10.5, { extraCost: new Float32Array(3) }); } catch (err) { threw = err instanceof TypeError; }
    assert(threw, 'a wrong-sized extraCost is a TypeError');

    // findPath charges the costs too: a dear block in the way is walked round.
    for (let z = 0; z < 20; z++) for (let x = 18; x < 22; x++) nav.setCellCost(x + 0.5, z + 0.5, 1);
    for (let z = 5; z < 15; z++) for (let x = 15; x < 25; x++) nav.setCellCost(x + 0.5, z + 0.5, 20);
    const path = nav.findPath(5.5, 10.5, 34.5, 10.5);
    assert(path.length >= 3, 'the route bends');
    assert(!path.some((q) => q.x > 15 && q.x < 25 && q.z > 5 && q.z < 15), 'and does not cross the mud');
}

// Blocked or off-grid goal: empty field.
nav.setWalkable(35.5, 10.5, false);
assert(nav.field(35.5, 10.5).reached === 0, 'blocked goal: nothing reached');
assert(nav.field(-3, 10.5).reached === 0, 'off-grid goal: nothing reached');
console.log('PASS');
