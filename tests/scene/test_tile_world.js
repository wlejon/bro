// Test scene.createTileWorld tile map authoring, rebuilding, and spatial queries
// Exercises src/js/tile_bindings.cpp and src/scene/tile_world.cpp

const canvas = document.createElement("canvas");
canvas.setAttribute("width", "128");
canvas.setAttribute("height", "128");
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext("scene");
if (!scene) {
    console.log("no scene context (no GPU); skipping tile_world test");
} else {
    assert(typeof scene.createTileWorld === "function", "createTileWorld is a function");

    // 1. Create TileWorld instance
    const world = scene.createTileWorld({
        width: 8,
        height: 8,
        layers: ["ground", "decor"],
        cellSize: 1.0,
        heightStep: 0.5,
        chunkSize: 4,
        palette: new Float32Array([
            0, 0, 0, 0,
            0.2, 0.7, 0.2, 1,
            0.7, 0.5, 0.2, 1,
        ]),
    });

    assert(world !== null && typeof world === "object", "createTileWorld returns world object");
    assert(world.width === 8, "world.width is 8");
    assert(world.height === 8, "world.height is 8");

    // 2. Tile and elevation authoring
    world.fillTile(0, 0, 7, 7, 1, 0);
    assert(world.getTile(3, 3, 0) === 1, "fillTile sets tile on ground layer");

    world.setTile(2, 2, 2, 0);
    assert(world.getTile(2, 2, 0) === 2, "setTile round-trips value");

    world.fillElevation(0, 0, 7, 7, 0);
    world.setElevation(4, 4, 3);
    assert(world.getElevation(4, 4) === 3, "setElevation round-trips elevation");

    // 3. Flags and tint
    world.setFlag(5, 5, 2, true);
    assert(world.hasFlag(5, 5, 2) === true, "setFlag sets flag bit");
    world.setFlag(5, 5, 2, false);
    assert(world.hasFlag(5, 5, 2) === false, "setFlag clears flag bit");

    world.setTint(1, 1, 1.0, 0.5, 0.5, 1.0);

    // 4. Rebuild mesh
    world.rebuild();
    assert(typeof world.chunkCount === "number" && world.chunkCount > 0, "chunkCount > 0 after rebuild");
    assert(typeof world.vertexCount === "number" && world.vertexCount > 0, "vertexCount > 0 after rebuild");
    assert(typeof world.triangleCount === "number" && world.triangleCount > 0, "triangleCount > 0 after rebuild");

    // 5. Spatial queries
    const cell = world.worldToCell(3.5, 3.5);
    assert(cell !== null && cell.x === 3 && cell.y === 3, "worldToCell maps coordinate to cell (3,3)");
    assert(world.worldToCell(-5, -5) === null, "worldToCell returns null for out-of-bounds");

    const center = world.cellCenterWorldXZ(3, 3);
    assert(Math.abs(center.x - 3.5) < 1e-4 && Math.abs(center.z - 3.5) < 1e-4, "cellCenterWorldXZ is (3.5, 3.5)");

    const bounds = world.worldBounds();
    assert(bounds.minX === 0 && bounds.maxX === 8 && bounds.minZ === 0 && bounds.maxZ === 8, "worldBounds matches grid extent");

    const height = world.sampleHeight(4.5, 4.5);
    assert(Math.abs(height - 3 * 0.5) < 1e-4, "sampleHeight matches elevation * heightStep");

    const rayHit = world.raycastCell([3.5, 10, 3.5], [0, -1, 0], 50);
    assert(rayHit !== null && rayHit.x === 3 && rayHit.y === 3, "raycastCell hits cell (3,3)");

    assert(world.isWalkable(3, 3) === true, "isWalkable is true for open cell");
}

document.body.removeChild(canvas);
console.log("test_tile_world: passed");
