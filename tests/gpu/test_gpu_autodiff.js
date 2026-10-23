// Tests for Issue #131: Symbolic Automatic Differentiation Compiler for
// @kernel / @shader JS Functions (bro.gpu.differentiate / bro.scene.compileGradShader)

assert(typeof bro === 'object', 'bro global exists');
assert(typeof bro.gpu.differentiate === 'function', 'bro.gpu.differentiate is a function');
assert(typeof bro.gpu.compileGradShader === 'function', 'bro.gpu.compileGradShader is a function');
assert(typeof bro.scene.differentiate === 'function', 'bro.scene.differentiate is a function');
assert(typeof bro.scene.compileGradShader === 'function', 'bro.scene.compileGradShader is a function');

// 1. 3D Polynomial & Trigonometric Analytical Gradient Accuracy across 100 coordinates
{
    const gradKernel = bro.gpu.differentiate(function polyTrig3D(x, y, z) {
        return x * x * y + Math.sin(z) * x - Math.exp(-y * z);
    });
    assert(gradKernel && gradKernel.ok === true, 'polyTrig3D autodiff compiled ok');
    assert(gradKernel.provenSafe === true, 'polyTrig3D provenSafe is true');
    assert(gradKernel.cpuGpuProvenEquivalent === true, 'polyTrig3D cpuGpuProvenEquivalent is true');
    assert(gradKernel.spirvWords instanceof Uint32Array && gradKernel.spirvWords.length > 5,
           'polyTrig3D produced valid SPIR-V binary');

    let count = 0;
    for (let ix = 0; ix < 5; ix++) {
        for (let iy = 0; iy < 5; iy++) {
            for (let iz = 0; iz < 4; iz++) {
                const x = -2.0 + 0.95 * ix;
                const y = -1.2 + 0.65 * iy;
                const z = -1.5 + 0.85 * iz;

                const expectedVal = x * x * y + Math.sin(z) * x - Math.exp(-y * z);
                const expectedDx = 2.0 * x * y + Math.sin(z);
                const expectedDy = x * x + z * Math.exp(-y * z);
                const expectedDz = x * Math.cos(z) + y * Math.exp(-y * z);

                const res = gradKernel.evalGrad(x, y, z);
                assert(Math.abs(res.value - expectedVal) < 5e-5, 'value match at ' + count);
                assert(Math.abs(res.dx - expectedDx) < 5e-5, 'dx match at ' + count);
                assert(Math.abs(res.dy - expectedDy) < 5e-5, 'dy match at ' + count);
                assert(Math.abs(res.dz - expectedDz) < 5e-5, 'dz match at ' + count);

                const nLen = Math.hypot(res.normal[0], res.normal[1], res.normal[2]);
                assert(Math.abs(nLen - 1.0) < 1e-5, 'unit normal length at ' + count);
                count++;
            }
        }
    }
    assert(count === 100, 'evaluated all 100 3D coordinates');
}

// 2. Procedural Terrain Heightfield Normal Compilation with smoothstep and clamp ridges
{
    const terrainShader = bro.scene.compileGradShader(function terrainHeight(x, z) {
        let sx = Math.sin(x * 0.25) * 8.0 + Math.cos(x * 0.5) * 2.0;
        let cz = Math.cos(z * 0.25) * 8.0 + Math.sin(z * 0.5) * 2.0;
        let ridge = clamp(Math.abs(sx - cz) * 0.35, 0.0, 5.0);
        let blend = smoothstep(-8.0, 8.0, sx + cz);
        return mix(sx + cz, ridge, blend);
    });
    assert(terrainShader && terrainShader.ok === true, 'terrainShader compiled ok');
    assert(terrainShader.isHeightfield2D === true, 'terrainShader detected 2D heightfield');
    assert(terrainShader.provenSafe === true, 'terrainShader provenSafe');

    const testCoords = [
        [0.0, 0.0],
        [Math.PI, Math.PI],
        [-2.0 * Math.PI, 0.0],
        [-15.0, -12.0],
        [-4.25, 7.75],
        [2.5, -3.5],
        [11.125, 14.875]
    ];
    for (const [x, z] of testCoords) {
        const g = terrainShader.evalGrad(x, z);
        assert(!Number.isNaN(g.value) && !Number.isNaN(g.dx) && !Number.isNaN(g.dz),
               'no NaN in heightfield value/gradient at (' + x + ',' + z + ')');
        const n = terrainShader.evalNormal(x, z);
        assert(!Number.isNaN(n[0]) && !Number.isNaN(n[1]) && !Number.isNaN(n[2]),
               'no NaN in heightfield normal at (' + x + ',' + z + ')');
        const len = Math.hypot(n[0], n[1], n[2]);
        assert(Math.abs(len - 1.0) < 1e-5 && n[1] > 0.0,
               'unit upward normal at (' + x + ',' + z + '): len=' + len);
    }

    if (bro.terrain && typeof bro.terrain.create === 'function') {
        const t = bro.terrain.create({ chunkSize: [16, 16, 16], cellSize: 1.0, loadRadius: 1 });
        t.setHeightSource(terrainShader);
        const tn = t.normalAt(2.5, -3.5);
        const sn = terrainShader.evalNormal(2.5, -3.5);
        assert(Math.abs(tn[0] - sn[0]) < 1e-5 &&
               Math.abs(tn[1] - sn[1]) < 1e-5 &&
               Math.abs(tn[2] - sn[2]) < 1e-5,
               'Terrain.normalAt returns exact analytical autodiff normal');
        t.destroy();
    }
}

// 3. Shared Subexpression GVN Compression on Math.sin(x*x + y*y + z*z)
{
    const radial = bro.gpu.differentiate(function radialWave(x, y, z) {
        return Math.sin(x * x + y * y + z * z);
    });
    assert(radial && radial.ok === true, 'radialWave compiled ok');
    assert(radial.sinCallCountBeforeGvn === 1, '1 sin call before GVN');
    assert(radial.cosCallCountBeforeGvn === 3, '3 cos calls before GVN');
    assert(radial.sinCallCountAfterGvn === 1, '1 sin call after GVN');
    assert(radial.cosCallCountAfterGvn === 1, '1 cos call after GVN (deduplicated from 3)');
    assert(radial.gvnExpressionsEliminated >= 2, 'GVN eliminated shared subexpressions');
}

// 4. CPU vs SPIR-V Equivalence & Batch Evaluation
{
    const sdf = bro.gpu.differentiate(function torusSdf(x, y, z) {
        let qx = Math.sqrt(x * x + z * z) - 2.0;
        return Math.sqrt(qx * qx + y * y) - 0.5;
    });
    assert(sdf && sdf.ok === true, 'torusSdf compiled ok');
    assert(sdf.cpuGpuProvenEquivalent === true, 'torusSdf CPU/SPIR-V proven equivalent');
    assert(sdf.provenSafe === true, 'torusSdf proven safe');
    assert(sdf.singleSourceParityVerified === true, 'torusSdf single-source parity verified');

    const c = sdf.evalGrad(3.0, 0.4, 4.0);
    const g = sdf.evalGradGpu(3.0, 0.4, 4.0);
    assert(Math.abs(c.value - g.value) < 1e-5, 'CPU vs SPIR-V value match');
    assert(Math.abs(c.dx - g.dx) < 1e-5, 'CPU vs SPIR-V dx match');
    assert(Math.abs(c.dy - g.dy) < 1e-5, 'CPU vs SPIR-V dy match');
    assert(Math.abs(c.dz - g.dz) < 1e-5, 'CPU vs SPIR-V dz match');

    const coords = new Float32Array([3.0, 0.4, 4.0, -2.0, 0.5, 1.5]);
    const batch = sdf.evalGradBatch(coords, 3);
    assert(batch instanceof Float32Array && batch.length === 14, 'evalGradBatch returns 7 floats per point');
    assert(Math.abs(batch[0] - c.value) < 1e-5 && Math.abs(batch[1] - c.dx) < 1e-5,
           'evalGradBatch point 0 matches evalGrad');
}

console.log('test_gpu_autodiff OK');
