// Test scene.createClipmapTerrain bindings and basic surface queries
// Exercises src/js/clipmap_bindings.cpp and src/scene/clipmap_terrain.cpp

const canvas = document.createElement("canvas");
canvas.setAttribute("width", "128");
canvas.setAttribute("height", "128");
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext("scene");
if (!scene) {
    console.log("no scene context (no GPU); skipping clipmap test");
} else {
    assert(typeof scene.createClipmapTerrain === "function", "createClipmapTerrain is a function");

    // 1. Create clipmap terrain
    const cm = scene.createClipmapTerrain({
        levels: 4,
        resolution: 32,
        cellSize: 1.0,
        detailRelief: 0.0,
    });

    assert(cm !== null && typeof cm === "object", "createClipmapTerrain returns object");
    assert(cm.levels === 4, "cm.levels is 4");
    assert(cm.resolution === 32, "cm.resolution is 32");
    assert(cm.cellSize === 1.0, "cm.cellSize is 1.0");
    assert(cm.layerCount === 0, "cm.layerCount is 0 initially");
    assert(typeof cm.vertexCount === "number" && cm.vertexCount > 0, "vertexCount > 0");
    assert(typeof cm.triangleCount === "number" && cm.triangleCount > 0, "triangleCount > 0");

    // 2. Set height layer with known linear field
    const W = 32, H = 32;
    const originX = -16, originZ = -16, mpc = 1.0;
    const f = (x, z) => 0.05 * x + 0.02 * z;

    const data = new Float32Array(W * H);
    for (let j = 0; j < H; j++) {
        for (let i = 0; i < W; i++) {
            data[j * W + i] = f(originX + i * mpc, originZ + j * mpc);
        }
    }

    cm.setHeightLayer(0, {
        data,
        width: W,
        height: H,
        originX,
        originZ,
        metresPerCell: mpc,
    });

    assert(cm.layerCount === 1, "layerCount is 1 after setHeightLayer");

    // 3. Elevation queries
    const testPoints = [
        [0, 0],
        [4, 4],
        [-8, 6],
        [10, -5],
    ];

    for (const [x, z] of testPoints) {
        const got = cm.elevationAt(x, z);
        const expected = f(x, z);
        assert(Math.abs(got - expected) < 1e-2, "elevationAt(" + x + "," + z + ") matches field: " + got + " vs " + expected);
    }
}

document.body.removeChild(canvas);
console.log("test_clipmap: passed");
