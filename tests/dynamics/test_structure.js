// A structure stands with no bodies, is met as a wall by what comes near, breaks where
// it is struck with enough, and what is left without a way to the ground falls.

const idle = (w, n) => { for (let i = 0; i < n; i++) w.step(1 / 60); };

// --- a wall on a footing -------------------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    w.addBox({ half: [60, 0.5, 60], position: [0, -1.3, 0], mass: 0 });
    const wall = w.addStructure({
        members: [
            { center: [0, -0.4, 0], half: [7, 0.4, 0.6], anchored: true },
            { center: [0, 4.8, 0], half: [6, 4.8, 0.15] },
        ],
        ties: [[0, 1]],
    });
    const built = wall.counts;
    assert(built.cells === 92, 'a 12 x 9.6 m wall in 1.2 m cells, and its footing: ' + built.cells);
    assert(wall.cells.length === 92 * 12, 'twelve numbers a cell');
    idle(w, 30);
    assert(wall.counts.standing === 92 && w.counts.bodies === 1, 'standing costs no bodies: ' + w.counts.bodies);

    // It has no bodies for the world's ray to find; the structure answers for what stands.
    assert(!w.raycast([2, 5, 10], [0, 0, -1], 50), 'the world sees nothing standing');
    const seen = wall.raycast([2, 5, 10], [0, 0, -1], 50);
    assert(seen && Math.abs(seen.distance - 9.85) < 0.01 && seen.normal[2] > 0.99, 'the wall is 9.85 m off, facing the ray: ' + JSON.stringify(seen));
    assert(wall.cells[seen.cell * 12] === 1, 'and the cell is one of the wall');
    assert(wall.raycast([2, 30, 10], [0, 0, -1], 50) === null, 'a ray over it misses');

    // Something slow is stopped by it, and it is none the worse.
    const slow = w.addSphere({ radius: 0.4, position: [-3, 3, 3], mass: 50, velocity: [0, 0, -3] });
    idle(w, 90);
    assert(w.pose(slow)[2] > 0.5, 'the slow ball stayed on its side: z ' + w.pose(slow)[2].toFixed(2));
    assert(wall.counts.standing === 92, 'and broke nothing');
    assert(wall.counts.held > 0, 'the wall had a body while the ball was there');

    // Five tonnes at 40 m/s go through, slower, with what they knocked out.
    const shot = w.addSphere({ radius: 0.5, position: [2, 5, 10], mass: 5000, velocity: [0, 0, -40] });
    let movedRows = 0;
    const bare = new Map();
    for (let i = 0; i < 60; i++) {
        w.step(1 / 60);
        movedRows = Math.max(movedRows, wall.moved().length / 8);
        const b = wall.bared();
        for (let k = 0; k < b.length; k += 2) bare.set(b[k], b[k + 1]);
    }
    // Every cell a neighbour parted from is told of, with the faces that left bare: the
    // wall is one cell thick along z, so nothing of it is bare on a z face.
    let standingBare = 0;
    const st = wall.state();
    for (const [cell, faces] of bare) {
        assert(faces > 0 && (wall.cells[cell * 12] !== 1 || (faces & 0x30) === 0), 'cell ' + cell + ' bare on ' + faces);
        if (st[cell] === -1) standingBare++;
    }
    assert(bare.size > 0 && standingBare > 0, 'the rim of the hole is bare: ' + standingBare + ' standing cells of ' + bare.size);
    const c = wall.counts;
    assert(c.standing < 92 && c.standing > 80, 'a hole, not the wall: ' + c.standing + ' standing');
    assert(c.pieces >= 1 && movedRows >= 1, 'what came out is loose: ' + c.pieces + ' bodies');
    assert(w.pose(shot)[2] < -1, 'the shot went through: z ' + w.pose(shot)[2].toFixed(2));
    const v = w.velocity(shot);
    assert(Math.hypot(v[0], v[1], v[2]) < 30, 'and it cost it: ' + Math.hypot(v[0], v[1], v[2]).toFixed(1) + ' m/s');
    const state = wall.state();
    let loose = 0;
    for (let i = 0; i < state.length; i++) if (state[i] >= 0) loose++;
    assert(loose === 92 - c.standing, 'state names the loose cells: ' + loose);
    const p = wall.pieces();
    assert(p.length === c.pieces * 3 && wall.pieceCells(p[0]).length === p[1], 'pieces and their cells agree');

    // A loose body dropped is gone, and its cells with it.
    wall.drop(p[0]);
    assert(!w.hasBody(p[0]) || wall.pieceCells(p[0]).length === 0, 'dropped');
    assert(wall.counts.pieces === c.pieces - 1, 'one piece fewer');
    wall.destroy();
    idle(w, 2);
    w.destroy();
}

// --- a slab on four columns ----------------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    w.addBox({ half: [60, 0.5, 60], position: [0, -1.3, 0], mass: 0 });
    const members = [{ center: [0, -0.4, 0], half: [5, 0.4, 5], anchored: true }];
    const ties = [];
    for (const [x, z] of [[-3, -3], [3, -3], [-3, 3], [3, 3]]) {
        members.push({ center: [x, 1.8, z], half: [0.25, 1.8, 0.25] });
        ties.push([0, members.length - 1], [members.length - 1, 5]);
    }
    members.push({ center: [0, 3.75, 0], half: [4, 0.15, 4] });
    const table = w.addStructure({ members, ties });
    const all = table.counts.cells;
    idle(w, 30);
    assert(table.counts.standing === all, 'the table stands: ' + table.counts.standing + ' of ' + all);

    // Both columns of one side go; the slab reaches too far from the two left and comes down.
    w.addSphere({ radius: 0.6, position: [-3, 1.8, 10], mass: 5000, velocity: [0, 0, -40] });
    idle(w, 240);
    const slab = table.cells;
    const state = table.state();
    let up = 0, cells = 0;
    for (let i = 0; i < state.length; i++) {
        if (slab[i * 12] !== 5) continue;
        cells++;
        if (state[i] === -1) up++;
    }
    assert(up < cells, 'slab cells came down: ' + (cells - up) + ' of ' + cells);
    w.destroy();
}

console.log('test_structure: PASS');
