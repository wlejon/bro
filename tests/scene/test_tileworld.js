// Test bro scene.createTileWorld — exercises src/scene/tile_world.cpp and
// src/js/tile_bindings.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping tileworld test');
} else if (typeof scene.createTileWorld !== 'function') {
    console.log('no createTileWorld; skipping');
} else {
    // ---- palette-mode world ------------------------------------------------
    const world = scene.createTileWorld({
        width: 8, height: 8,
        layers: ['ground', 'overlay'],
        cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
        palette: new Float32Array([
            0, 0, 0, 0,
            0.3, 0.6, 0.3, 1,
            0.6, 0.5, 0.3, 1,
        ]),
    });
    assert(world !== null, 'createTileWorld returns object');
    assert(world.width === 8, 'width reflects config');
    assert(world.height === 8, 'height reflects config');

    // Authoring round-trip.
    world.fillTile(0, 0, 7, 7, 1, 0);
    assert(world.getTile(3, 3, 0) === 1, 'fillTile sets ground layer');
    world.setTile(2, 2, 2, 0);
    assert(world.getTile(2, 2, 0) === 2, 'setTile round-trips');

    world.fillElevation(0, 0, 7, 7, 0);
    world.setElevation(4, 4, 3);
    assert(world.getElevation(4, 4) === 3, 'setElevation round-trips');

    world.setFlag(5, 5, 1, true);
    assert(world.hasFlag(5, 5, 1) === true, 'setFlag/hasFlag round-trip');
    world.setFlag(5, 5, 1, false);
    assert(world.hasFlag(5, 5, 1) === false, 'setFlag clears bit');

    world.setTint(1, 1, 1, 0, 0, 1);
    world.fillTint(6, 6, 7, 7, 0, 1, 0, 1);

    world.rebuild();
    assert(typeof world.chunkCount === 'number' && world.chunkCount > 0, 'chunkCount > 0 after rebuild');
    assert(typeof world.vertexCount === 'number' && world.vertexCount > 0, 'vertexCount > 0');
    assert(typeof world.triangleCount === 'number' && world.triangleCount > 0, 'triangleCount > 0');

    // worldToCell.
    const cell = world.worldToCell(3.5, 3.5);
    assert(cell !== null && cell.x === 3 && cell.y === 3, 'worldToCell maps world->cell');
    assert(world.worldToCell(-10, -10) === null, 'worldToCell out of bounds -> null');

    // cellCenterWorldXZ / worldBounds (square).
    const center33 = world.cellCenterWorldXZ(3, 3);
    assert(Math.abs(center33.x - 3.5) < 1e-4 && Math.abs(center33.z - 3.5) < 1e-4,
        'cellCenterWorldXZ matches square cell center');
    const sqBounds = world.worldBounds();
    assert(Math.abs(sqBounds.minX) < 1e-4 && Math.abs(sqBounds.minZ) < 1e-4 &&
        Math.abs(sqBounds.maxX - 8) < 1e-4 && Math.abs(sqBounds.maxZ - 8) < 1e-4,
        'worldBounds matches square grid extent');

    // raycastCell: straight down onto a known-solid, flat (elevation 0) cell.
    const hit = world.raycastCell([3.5, 10, 3.5], [0, -1, 0], 100);
    assert(hit !== null, 'raycastCell hits a solid cell from above');
    assert(hit.x === 3 && hit.y === 3, 'raycastCell resolves the correct cell');
    assert(hit.side === false, 'straight-down ray hits the top face, not a cliff');

    // sampleHeight matches the authored elevation at (4,4).
    const y = world.sampleHeight(4.5, 4.5);
    assert(Math.abs(y - 3 * 0.5) < 1e-4, 'sampleHeight matches elevation*heightStep');

    // isWalkable / nav-grid interop.
    assert(world.isWalkable(3, 3) === true, 'solid cell is walkable');
    world.setFlag(3, 3, 4, true);
    assert(world.isWalkable(3, 3, 4) === false, 'blockMask makes flagged cell unwalkable');

    const nav = world.toNavGrid({ blockMask: 4 });
    assert(nav !== null, 'toNavGrid returns a nav grid');
    assert(nav.isWalkable(3, 3) === false, 'nav grid reflects blocked cell');
    assert(nav.isWalkable(0, 0) === true, 'nav grid reflects open cell');

    const G = bro.ai.game;
    const nav2 = G.createNavGrid({ minX: 0, minZ: 0, maxX: 8, maxZ: 8, cellSize: 1 });
    const blocked = world.syncNavGrid(nav2, { blockMask: 4 });
    assert(typeof blocked === 'number' && blocked >= 1, 'syncNavGrid reports blocked-cell count');

    world.setFlag(3, 3, 4, false);

    // objects.
    const cone = Mesh.cone(0.3, 0.6, 8, 1, true);
    const treeKind = world.addObjectKind(cone, { color: [0.2, 0.5, 0.2, 1] });
    assert(treeKind >= 0, 'addObjectKind returns a kind index');
    world.addObject(treeKind, 1, 1, { yaw: 0.5, scale: 1.2 });
    world.addObject(treeKind, 2, 1, {});
    assert(world.objectCount(treeKind) === 2, 'objectCount tracks placements');
    world.rebuildObjects();
    world.clearObjects(treeKind);
    assert(world.objectCount(treeKind) === 0, 'clearObjects empties the kind');

    // save/load round trip.
    world.setTile(0, 0, 9, 0);
    const before = world.getTile(0, 0, 0);
    const bytes = world.save();
    assert(bytes instanceof Uint8Array, 'save returns a Uint8Array');
    assert(bytes.length > 0, 'save returns non-empty bytes');
    world.setTile(0, 0, 0, 0);   // mutate away from the saved state
    assert(world.getTile(0, 0, 0) !== before, 'mutation actually changed state');
    const ok = world.load(bytes);
    assert(ok === true, 'load returns true on valid bytes');
    assert(world.getTile(0, 0, 0) === before, 'load restores saved tile state');
    assert(world.load(new Uint8Array([1, 2, 3])) === false, 'load returns false on corrupt bytes');

    // reconfigure.
    world.configure({ width: 6, height: 6, layers: ['ground'] });
    assert(world.width === 6 && world.height === 6, 'configure reconfigures dimensions');

    world.destroy();

    // ---- atlas-mode world (autotile / overlay / animation code paths) -----
    const atlasCols = 4, atlasRows = 4;
    const atlasW = atlasCols * 16, atlasH = atlasRows * 16;
    const atlasPixels = new Uint8Array(atlasW * atlasH * 4).fill(200);

    const edgeVariants = [];
    for (let i = 0; i < 16; i++) edgeVariants.push(i % (atlasCols * atlasRows));

    const atlasWorld = scene.createTileWorld({
        width: 8, height: 8,
        layers: ['ground', 'overlay'],
        cellSize: 1.0, chunkSize: 4,
        atlasPixels, atlasWidth: atlasW, atlasHeight: atlasH,
        atlasColumns: atlasCols, atlasRows: atlasRows,
        autotiles: [
            { id: 1, layer: 0, mode: 'edge', cells: edgeVariants },
        ],
        overlays: [{}, { opacity: 0.8, alphaCutoff: 0.1 }],
        animations: [{ id: 2, fps: 4, frames: [0, 1, 2, 3] }],
    });
    assert(atlasWorld !== null, 'atlas-mode createTileWorld succeeds');

    atlasWorld.fillTile(0, 0, 7, 7, 1, 0);   // autotiled ground
    atlasWorld.setTile(3, 3, 2, 1);          // animated overlay tile
    atlasWorld.rebuild();
    assert(atlasWorld.chunkCount > 0, 'atlas world builds chunks with autotile+overlay');

    const advanced = atlasWorld.advance(500);
    assert(typeof advanced === 'boolean', 'advance returns boolean');

    atlasWorld.destroy();

    // ---- hex-topology world ------------------------------------------------
    const hexWorld = scene.createTileWorld({
        width: 6, height: 6,
        topology: 'hex',
        cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
        palette: new Float32Array([
            0, 0, 0, 0,
            0.3, 0.6, 0.3, 1,
        ]),
    });
    assert(hexWorld !== null, 'hex createTileWorld succeeds');
    assert(hexWorld.width === 6 && hexWorld.height === 6, 'hex world reflects dimensions');

    hexWorld.fillTile(0, 0, 5, 5, 1, 0);
    hexWorld.rebuild();
    const flatVerts = hexWorld.vertexCount;
    const flatTris = hexWorld.triangleCount;
    assert(flatVerts > 0, 'hex world produces geometry (flat field)');
    assert(flatTris > 0, 'hex world produces triangles (flat field)');

    // Raising one cell should add cliff geometry (more verts/tris than a flat field).
    hexWorld.setElevation(2, 2, 2);
    hexWorld.rebuild();
    assert(hexWorld.vertexCount > flatVerts, 'raising a cell adds cliff vertices');
    assert(hexWorld.triangleCount > flatTris, 'raising a cell adds cliff triangles');

    // raycastCell straight down onto known cell centers, for cells whose world
    // center is computable via the JS-visible axial formula (mirrors
    // TileWorld::cellCenterLocal's hex branch).
    const R = 1.0;
    const sqrt3 = Math.sqrt(3);
    function hexCenter(x, y) {
        const q = x - (y - (y & 1)) / 2;
        const r = y;
        return { px: R * sqrt3 * (q + r * 0.5), pz: R * 1.5 * r };
    }
    for (const [hx, hy] of [[0, 0], [3, 2], [5, 5], [1, 4]]) {
        const c = hexCenter(hx, hy);
        const hHit = hexWorld.raycastCell([c.px, 10, c.pz], [0, -1, 0], 100);
        assert(hHit !== null, `hex raycastCell hits cell (${hx},${hy})`);
        assert(hHit.x === hx && hHit.y === hy, `hex raycastCell resolves cell (${hx},${hy}), got (${hHit && hHit.x},${hHit && hHit.y})`);

        // cellCenterWorldXZ should agree with the JS-side axial formula.
        const cc = hexWorld.cellCenterWorldXZ(hx, hy);
        assert(Math.abs(cc.x - c.px) < 1e-3 && Math.abs(cc.z - c.pz) < 1e-3,
            `hex cellCenterWorldXZ matches axial formula for (${hx},${hy})`);
    }

    // worldBounds (hex): sweeps actual hex corners, so it should extend past
    // every tested cell center by at least the hex circumradius R on each side.
    const hexBounds = hexWorld.worldBounds();
    assert(hexBounds.minX < 0 - 1e-3 && hexBounds.minZ < 0 - 1e-3,
        'hex worldBounds min extends past cell (0,0) center for the hex corners');
    const farCenter = hexCenter(5, 5);
    assert(hexBounds.maxX > farCenter.px && hexBounds.maxZ > farCenter.pz,
        'hex worldBounds max extends past the farthest cell center');

    // Cliff hit: a ray descending just past the raised cell's edge should strike
    // its cliff face, not the flat neighbour top.
    const raised = hexCenter(2, 2);
    const cliffHit = hexWorld.raycastCell([raised.px + 0.85, 10, raised.pz], [0, -1, 0], 100);
    assert(cliffHit !== null, 'hex raycastCell hits near a cliff edge');

    // Object placement on a hex world (smoke test the hex branch of rebuildObjectKind).
    const hexCone = Mesh.cone(0.2, 0.4, 6, 1, true);
    const hexKind = hexWorld.addObjectKind(hexCone, { color: [0.6, 0.4, 0.2, 1] });
    hexWorld.addObject(hexKind, 1, 1, {});
    assert(hexWorld.objectCount(hexKind) === 1, 'hex world addObject/objectCount round-trip');
    hexWorld.rebuildObjects();

    // Nav-grid interop (worldBounds + cellCenterWorldXZ). NavGrid.isWalkable takes
    // WORLD-space coordinates, so query at the hex cells' actual pixel centers.
    hexWorld.setFlag(2, 2, 1, true);
    assert(hexWorld.isWalkable(2, 2, 1) === false, 'hex isWalkable respects blockMask');
    const hexNav = hexWorld.toNavGrid({ blockMask: 1 });
    assert(hexNav !== null, 'hex toNavGrid returns a nav grid');
    assert(hexNav.isWalkable(raised.px, raised.pz) === false, 'hex nav grid reflects blocked cell');
    const openCenter = hexCenter(0, 0);
    assert(hexNav.isWalkable(openCenter.px, openCenter.pz) === true, 'hex nav grid reflects open cell');

    // ---- grid search / regions / coordinate math ---------------------------
    // A fresh 8x8 square world: ground everywhere, a wall of flagged cells at
    // x==4 with a gap at y==6, and a dirt (id 2) patch for region queries.
    const WALL = 4;
    const sw = scene.createTileWorld({ width: 8, height: 8, chunkSize: 4 });
    sw.fillTile(0, 0, 7, 7, 1);
    for (let y = 0; y < 8; y++) if (y !== 6) sw.setFlag(4, y, WALL, true);
    sw.fillTile(1, 1, 2, 2, 2);            // 2x2 dirt region

    // findPath routes through the gap.
    const path = sw.findPath(0, 0, 7, 0, { blockMask: WALL });
    assert(path.length > 0, 'findPath finds a route');
    assert(path[0].x === 0 && path[0].y === 0, 'findPath starts at start');
    assert(path[path.length - 1].x === 7 && path[path.length - 1].y === 0, 'findPath ends at goal');
    assert(path.some((c) => c.x === 4 && c.y === 6), 'findPath threads the wall gap');
    assert(path.length >= 15, 'findPath detours around the wall');

    // Blocking the gap makes the goal unreachable.
    sw.setFlag(4, 6, WALL, true);
    assert(sw.findPath(0, 0, 7, 0, { blockMask: WALL }).length === 0,
        'findPath returns [] when sealed off');
    sw.setFlag(4, 6, WALL, false);

    // Terrain costs: dirt (id 2) cost 10 pushes the path around the patch.
    const costly = sw.findPath(0, 0, 3, 3, { blockMask: WALL, costs: [0, 1, 10] });
    assert(costly.length > 0, 'findPath with costs finds a route');
    assert(!costly.some((c) => sw.getTile(c.x, c.y) === 2),
        'findPath avoids expensive terrain');

    // distanceField: -1 behind the wall side is reachable only via the gap.
    const field = sw.distanceField([{ x: 0, y: 0 }], { blockMask: WALL });
    assert(field instanceof Int32Array && field.length === 64, 'distanceField shape');
    assert(field[0] === 0, 'distanceField source is 0');
    assert(field[6 * 8 + 4] > 0, 'distanceField reaches the gap');
    assert(field[0 * 8 + 7] === field[6 * 8 + 4] + 1 + 6 + 3 || field[0 * 8 + 7] > 10,
        'distanceField far side routes through the gap');
    assert(field[3 * 8 + 4] === -1, 'distanceField wall cells are -1');

    // floodFill: the dirt patch is one 4-cell region.
    const dirt = sw.floodFill(1, 1);
    assert(dirt.length === 4, 'floodFill same-tile default finds the dirt patch');
    const flagged = sw.floodFill(4, 0, { flag: WALL });
    assert(flagged.length === 6, 'floodFill flag match walks the wall segment');

    // components: dirt forms exactly one component; grass one (all connected).
    const dirtComps = sw.components({ id: 2 });
    assert(dirtComps.length === 1 && dirtComps[0].length === 4, 'components finds one dirt region');

    // Coordinate math.
    assert(sw.cellDistance(0, 0, 3, 4) === 7, 'cellDistance Manhattan');
    assert(sw.cellDistance(0, 0, 3, 4, 'vertex') === 4, 'cellDistance Chebyshev');
    assert(sw.cellRing(3, 3, 1).length === 4, 'cellRing edge radius 1 -> 4 cells');
    assert(sw.cellRing(3, 3, 1, 'vertex').length === 8, 'cellRing vertex radius 1 -> 8 cells');
    assert(sw.cellRing(0, 0, 1).length === 2, 'cellRing clips out-of-bounds');
    assert(sw.cellsInRange(3, 3, 1).length === 5, 'cellsInRange radius 1 -> 5 cells');
    const lineCells = sw.cellLine(0, 0, 3, 2);
    assert(lineCells.length >= 4, 'cellLine spans the segment');
    assert(lineCells[0].x === 0 && lineCells[0].y === 0, 'cellLine starts at a');
    assert(lineCells[lineCells.length - 1].x === 3 && lineCells[lineCells.length - 1].y === 2,
        'cellLine ends at b');
    sw.destroy();

    // Hex variants: 6-way connectivity and cube-metric distance.
    const hw = scene.createTileWorld({ width: 8, height: 8, topology: 'hex', chunkSize: 4 });
    hw.fillTile(0, 0, 7, 7, 1);
    assert(hw.cellRing(3, 3, 1).length === 6, 'hex cellRing radius 1 -> 6 cells');
    assert(hw.cellsInRange(3, 3, 1).length === 7, 'hex cellsInRange radius 1 -> 7 cells');
    assert(hw.cellDistance(3, 3, 3, 3) === 0, 'hex cellDistance identity');
    const hexPath = hw.findPath(0, 0, 7, 7);
    assert(hexPath.length > 0, 'hex findPath finds a route');
    assert(hexPath.length - 1 === hw.cellDistance(0, 0, 7, 7),
        'hex findPath length matches hex distance on open ground');
    const hexField = hw.distanceField({ x: 0, y: 0 });
    assert(hexField[7 * 8 + 7] === hw.cellDistance(0, 0, 7, 7),
        'hex distanceField matches hex distance');
    assert(hw.cellNeighbors(3, 3).length === 6, 'hex cellNeighbors -> 6 cells');
    hw.destroy();

    // ---- bugfix-pass regressions -------------------------------------------
    const bw = scene.createTileWorld({ width: 8, height: 8, chunkSize: 4 });
    bw.fillTile(0, 0, 7, 7, 1);
    bw.fillTile(1, 1, 2, 2, 2);            // dirt patch, ids as above

    // isWalkable blockMask is ANY-bit, matching findPath/distanceField: a cell
    // flagged 4 must be blocked by mask 7 even though (flags & 7) != 7.
    bw.setFlag(5, 5, 4, true);
    assert(bw.isWalkable(5, 5, 7) === false, 'isWalkable multi-bit mask blocks on ANY shared bit');
    assert(bw.isWalkable(5, 5, 2) === true, 'isWalkable mask with no shared bit stays walkable');
    assert(bw.isWalkable(5, 5, 4) === false, 'isWalkable exact bit blocks');
    bw.setFlag(5, 5, 4, false);

    // Weighted distanceField: costs switch to Dijkstra + Float32Array.
    const wf = bw.distanceField({ x: 0, y: 0 }, { costs: [0, 1, 10] });
    assert(wf instanceof Float32Array && wf.length === 64, 'weighted distanceField is Float32Array');
    assert(wf[0] === 0, 'weighted field source is 0');
    assert(wf[0 * 8 + 1] === 1, 'weighted field grass step costs 1');
    assert(wf[1 * 8 + 1] === 11, 'weighted field pays the dirt entry cost');
    assert(wf[2 * 8 + 2] === 15, 'weighted field routes around the dirt patch');
    const uf = bw.distanceField({ x: 0, y: 0 });
    assert(wf[2 * 8 + 2] > uf[2 * 8 + 2], 'weighted field exceeds uniform BFS over costly terrain');

    // A source on an impassable cell seeds 0 and spreads to its neighbours.
    bw.setFlag(3, 3, 4, true);
    const bf = bw.distanceField({ x: 3, y: 3 }, { blockMask: 4 });
    assert(bf[3 * 8 + 3] === 0, 'blocked source seeds distance 0');
    assert(bf[3 * 8 + 4] === 1, 'blocked source spreads to passable neighbours');
    bw.setFlag(3, 3, 4, false);

    // getTint round-trips the 8-bit-quantized values; clamps beyond 0..1.
    bw.setTint(2, 2, 0.5, 0.25, 1.0, 1.0);
    const t = bw.getTint(2, 2);
    assert(Math.abs(t.r - 0.5) < 0.003 && Math.abs(t.g - 0.25) < 0.003 &&
           t.b === 1 && t.a === 1, 'getTint returns the quantized stored tint');
    bw.setTint(2, 3, 2.0, -1.0, 0.5);
    const tc = bw.getTint(2, 3);
    assert(tc.r === 1 && tc.g === 0, 'setTint clamps channels to 0..1');
    const tu = bw.getTint(6, 6);
    assert(tu.r === 1 && tu.g === 1 && tu.b === 1 && tu.a === 1, 'untinted cell reads white');
    const to = bw.getTint(-3, 99);
    assert(to.r === 1 && to.g === 1 && to.b === 1 && to.a === 1, 'OOB getTint reads white');

    // cellNeighbors: topology + conn aware, in-bounds only, canonical order.
    assert(bw.cellNeighbors(3, 3).length === 4, 'square edge cellNeighbors -> 4');
    assert(bw.cellNeighbors(3, 3, 'vertex').length === 8, 'square vertex cellNeighbors -> 8');
    assert(bw.cellNeighbors(0, 0).length === 2, 'corner cellNeighbors clips to in-bounds');

    // load() preserves registered object kinds (instances cleared, ids valid).
    const bwCone = Mesh.cone(0.2, 0.4, 6, 1, true);
    const bwKind = bw.addObjectKind(bwCone, { color: [1, 1, 1, 1] });
    assert(bwKind >= 0, 'addObjectKind registers');
    bw.addObject(bwKind, 1, 1, {});
    assert(bw.objectCount(bwKind) === 1, 'instance placed before save');
    const bwBytes = bw.save();
    bw.setTile(0, 0, 2);
    assert(bw.load(bwBytes) === true, 'load round-trips');
    assert(bw.getTile(0, 0) === 1, 'load restored the grid');
    assert(bw.objectCount(bwKind) === 0, 'load cleared instance placements');
    const rePlaced = bw.addObject(bwKind, 1, 1, {});
    assert(rePlaced >= 0, 'kind id survives load — addObject works without re-registering');
    assert(bw.objectCount(bwKind) === 1, 'instance re-placed after load');
    bw.rebuildObjects();
    bw.destroy();

    hexWorld.destroy();
    console.log('tileworld: all assertions passed');
}

document.body.removeChild(canvas);
