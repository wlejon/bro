// grid.* helpers: ObsWindow, FrameStack, FailureTape, BestCrop, PotentialShaper, StallDetector.

const grid = bro.ai.game.grid;

// --- ObsWindow ---
{
    const ow = grid.createObsWindow({
        spec: { colsBehind: 1, colsAhead: 2, rowsUp: 1, rowsDown: 1, tileChannels: 1, selfBlockSize: 2 },
        tile: { sample(c, r) { return 0.5; } },
        layers: [],
    });
    assert(typeof ow.outDim === 'number', 'outDim is number, got ' + typeof ow.outDim);
    const cols = 1 + 2 + 1, rows = 1 + 1 + 1; // colsBehind + colsAhead + center=1
    // The exact layout (cols total) isn't fully specced; just check outDim > 0.
    assert(ow.outDim > 0, 'outDim positive, got ' + ow.outDim);

    const layout = ow.layout();
    assert(layout && typeof layout === 'object', 'layout is object');
    assert('total' in layout, 'layout has total');
    assert(layout.total === ow.outDim, 'layout.total == outDim');

    const buf = ow.build(0, 0, new Float32Array([1.0, 2.0]));
    assert(buf instanceof Float32Array, 'build returns Float32Array');
    assert(buf.length === ow.outDim, 'build buf len = outDim, got ' + buf.length);
}

// --- FrameStack ---
{
    const fs = grid.createFrameStack({ innerDim: 4, k: 3 });
    assert(fs.outDim === 12, 'frameStack outDim = innerDim*k = 12, got ' + fs.outDim);
    assert(fs.filled === 0, 'initial filled=0, got ' + fs.filled);
    fs.push(new Float32Array([1, 1, 1, 1]));
    assert(fs.filled === 1, 'filled=1 after one push');
    fs.push(new Float32Array([2, 2, 2, 2]));
    fs.push(new Float32Array([3, 3, 3, 3]));
    fs.push(new Float32Array([4, 4, 4, 4]));
    assert(fs.filled === 3, 'filled clamped to k=3, got ' + fs.filled);
    const stacked = fs.read();
    assert(stacked instanceof Float32Array && stacked.length === 12, 'stacked len 12');
    // Chronological [oldest..newest]; oldest now is the "2" frame.
    assert(stacked[0] === 2, 'oldest frame value 2, got ' + stacked[0]);
    assert(stacked[11] === 4, 'newest frame value 4, got ' + stacked[11]);
    fs.reset();
    assert(fs.filled === 0, 'reset zeroes filled');
}

// --- FailureTape ---
{
    const tape = grid.createFailureTape({ tapeDepth: 8, ringCapacity: 32, penalty: 0.5, floor: 0.05 });
    tape.recordFailure([{ sig: 'S1', action: 1 }, { sig: 'S2', action: 0 }]);
    const m = tape.multipliers('S1', 3);
    assert(m instanceof Float32Array, 'multipliers is Float32Array');
    assert(m.length === 3, 'multipliers len = numActions, got ' + m.length);
    assert(m[1] < 1.0, 'recorded action gets penalty, got m[1]=' + m[1]);
    assert(m[0] === 1.0 || Math.abs(m[0] - 1) < 1e-5, 'non-recorded action unmodified, got m[0]=' + m[0]);
    assert(m[1] >= 0.05, 'penalty floored at 0.05, got ' + m[1]);

    const adj = tape.applyPriors('S1', [0.5, 0.3, 0.2]);
    assert(adj && adj.length === 3, 'applyPriors returns len-3');
    assert(adj[1] < 0.3, 'penalized action shrunk: ' + adj[1] + ' vs 0.3');
}

// --- BestCrop ---
{
    const crop = grid.createBestCrop({
        capacity: 4, depthBonus: 0.01, ageDecay: 0.001, seedTopK: 2, seed: 1n,
    });
    crop.push({ snapshot: { tag: 'a' }, prefix: [0, 1], score: 1.0, depth: 5 });
    crop.push({ snapshot: { tag: 'b' }, prefix: [2],    score: 2.0, depth: 3 });
    assert(crop.size === 2, 'crop.size=2, got ' + crop.size);
    const picked = crop.seed();
    assert(picked && picked.snapshot, 'seed returns object with snapshot');
    // prefix is returned as Int32Array (per binding).
    assert(picked.prefix instanceof Int32Array || Array.isArray(picked.prefix),
        'seed.prefix is Int32Array/Array, got ' + Object.prototype.toString.call(picked.prefix));
}

// --- PotentialShaper ---
{
    const sh = grid.createPotentialShaper({ gamma: 0.99 });
    sh.reset(-1.0);
    const bonus = sh.step(-0.5);   // phi went up
    assert(typeof bonus === 'number' && Number.isFinite(bonus), 'shaper bonus finite, got ' + bonus);
    // bonus = gamma * phi' - phi = 0.99 * -0.5 - (-1.0) = 0.505
    assert(bonus > 0, 'progress yields positive bonus, got ' + bonus);
}

// --- StallDetector ---
{
    const sd = grid.createStallDetector({ epsilon: 0.5, patience: 5 });
    sd.reset();
    let fired = false;
    for (let i = 0; i < 20; i++) {
        if (sd.tick(0.0)) { fired = true; break; }
    }
    assert(fired, 'stall detector fires on flat progress');

    const sd2 = grid.createStallDetector({ epsilon: 0.1, patience: 5 });
    sd2.reset();
    let firedFast = false;
    for (let i = 0; i < 20; i++) {
        if (sd2.tick(i * 10.0)) { firedFast = true; break; }
    }
    assert(!firedFast, 'stall detector does NOT fire on rising progress');
}

console.log('test_grid: OK');
