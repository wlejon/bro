// Integration test for single-source JS @kernel / @shader compilation (#116)
// and Vulkan GPU SDF compute dispatch & meshing (#115) in bro.gpu and bro.scene.

// 1. Single-source 2D scalar height kernel compiled from a live JS function
{
    function terrainWave(x, z) {
        return Math.sin(x * 0.25) * 2.0 + Math.cos(z * 0.25) * 1.5;
    }

    const k = bro.gpu.compileKernel(terrainWave);
    assert(k && k.ok === true, 'bro.gpu.compileKernel(terrainWave) succeeds');
    assert(k.kernelName === 'terrainWave', `kernelName is terrainWave (got ${k.kernelName})`);
    assert(k.paramCount === 2, `paramCount is 2 (got ${k.paramCount})`);
    assert(k.cpuGpuProvenEquivalent === true, 'CPU MIR == SPIR-V equivalence is formally proven');
    assert(k.singleSourceParityVerified === true, 'single-source 64-point parity is verified');
    assert(k.maxCpuGpuUlpDiff <= 2.0, `maxCpuGpuUlpDiff <= 2.0 (got ${k.maxCpuGpuUlpDiff})`);
    assert(k.relaxedPrecisionPromotedCount > 0,
           `RelaxedPrecision promoted bounded ops (got ${k.relaxedPrecisionPromotedCount})`);
    assert(k.spirvWords instanceof Uint32Array && k.spirvWords.length > 20,
           'spirvWords is a non-empty Uint32Array');
    assert(k.spirvWords[0] === 0x07230203, 'spirvWords starts with SPIR-V magic 0x07230203');
    assert(typeof k.ptx === 'string' && k.ptx.includes('.visible .entry terrainWave'),
           'ptx contains .visible .entry terrainWave');

    // Verify evalCpu vs evalGpu parity on the live Vulkan GPU
    const cpuVal = k.evalCpu(1.5, -0.75);
    const gpuVal = k.evalGpu(1.5, -0.75);
    const refVal = Math.sin(1.5 * 0.25) * 2.0 + Math.cos(-0.75 * 0.25) * 1.5;
    assert(Math.abs(cpuVal - refVal) < 1e-5, `evalCpu matches JS math (${cpuVal} vs ${refVal})`);
    assert(Math.abs(gpuVal - cpuVal) < 1e-5, `evalGpu matches evalCpu (${gpuVal} vs ${cpuVal})`);
    if (bro.gpu.vulkanComputeAvailable()) {
        assert(k.lastUsedVulkanGpu === true, 'evalGpu executed on the hardware Vulkan compute queue');
    }

    // Verify Terrain.prototype.setHeightSource accepts a compiled @kernel object directly
    const terrain = new bro.scene.Terrain();
    terrain.configure({
        chunkSize: [16, 32, 16],
        cellSize: 1.0,
        loadRadius: 1,
        unloadRadius: 2
    });
    terrain.setHeightSource(k);
    const loaded = terrain.update(0, 0, 0);
    assert(loaded > 0, `terrain loaded chunks using compiled @kernel height source (${loaded})`);
    assert(terrain.vertexCount > 0, `terrain generated vertices from @kernel height source (${terrain.vertexCount})`);
    terrain.destroy();
}

// 2. Arrow function compilation via bro.scene.compileShader & 1D buffer dispatch on Vulkan
{
    const waveShader = bro.scene.compileShader((x, scale) => Math.sin(x * 0.5) * scale + 1.0);
    assert(waveShader && waveShader.ok === true, 'bro.scene.compileShader on arrow function succeeds');
    assert(waveShader.cpuGpuProvenEquivalent === true, 'arrow function CPU == GPU equivalence proven');

    const outArr = new Float32Array(16);
    waveShader.dispatch1D(outArr, 3.0);
    for (let i = 0; i < 16; i++) {
        const expected = Math.sin(i * 0.5) * 3.0 + 1.0;
        assert(Math.abs(outArr[i] - expected) < 1e-4,
               `dispatch1D element ${i} matches expected (${outArr[i]} vs ${expected})`);
    }
}

// 3. Explicit buffer compute kernel (Float32Array storage buffers) dispatched on Vulkan
{
    const saxpyKernel = bro.gpu.compileKernel(`
        // @kernel
        function saxpy(out, a, scale, n) {
            let i = global_tid_x();
            if (i < n) {
                out[i] = a[i] * scale + 2.5;
            }
        }
    `);
    assert(saxpyKernel && saxpyKernel.ok === true, 'buffer compute kernel saxpy compiles cleanly');

    const N = 64;
    const a = new Float32Array(N);
    const out = new Float32Array(N);
    for (let i = 0; i < N; i++) a[i] = i * 0.5;

    saxpyKernel.dispatch1D(out, a, 4.0, N);
    if (bro.gpu.vulkanComputeAvailable()) {
        assert(saxpyKernel.lastUsedVulkanGpu === true,
               'buffer compute kernel saxpy dispatched on real Vulkan queue');
        for (let i = 0; i < N; i++) {
            const expected = (i * 0.5) * 4.0 + 2.5;
            assert(Math.abs(out[i] - expected) < 1e-4,
                   `Vulkan saxpy out[${i}] = ${out[i]} matches ${expected}`);
        }
    }
}

// 4. SDFGraph.toMesh(), evaluateGridGpu(), traceRaysGpu(), and renderToBuffer() (#115)
{
    const g = new SDFGraph();
    const s = g.sphere(1.2);
    const b = g.box(0.9, 0.9, 0.9);
    const root = g.opSmoothUnion(s, b, 0.25);
    g.setRoot(root);

    // .toMesh() helper (marchingCubes default + surfaceNets mode)
    const mcMesh = g.toMesh({ resolution: 24, bounds: [[-2, -2, -2], [2, 2, 2]] });
    assert(mcMesh && mcMesh.vertexCount > 0 && mcMesh.triangleCount > 0,
           `SDFGraph.toMesh() marchingCubes produced mesh (${mcMesh.vertexCount} verts)`);
    const snMesh = g.toMesh({ mode: 'surfaceNets', resolution: 24, bounds: [[-2, -2, -2], [2, 2, 2]] });
    assert(snMesh && snMesh.vertexCount > 0 && snMesh.triangleCount > 0,
           `SDFGraph.toMesh({mode:'surfaceNets'}) produced mesh (${snMesh.vertexCount} verts)`);

    // .evaluateGridGpu() on Vulkan
    const gridRes = g.evaluateGridGpu({
        resolution: 16,
        boundsMin: [-2, -2, -2],
        boundsMax: [2, 2, 2]
    });
    assert(gridRes && gridRes.field instanceof Float32Array && gridRes.field.length === 4096,
           'SDFGraph.evaluateGridGpu returns 16^3 Float32Array');
    if (bro.scene.sdfVulkanAvailable()) {
        assert(gridRes.usedVulkanGpu === true, 'evaluateGridGpu ran on hardware Vulkan queue');
    }

    // .renderToBuffer() / bro.scene.renderSdfToBuffer() on Vulkan
    const frame = bro.scene.renderSdfToBuffer(g, {
        width: 16,
        height: 16,
        eye: [0, 0, 4],
        target: [0, 0, 0],
        fovY: 50
    });
    assert(frame && frame.hits instanceof Float32Array && frame.hits.length === 16 * 16 * 8,
           'renderSdfToBuffer returns 16x16x8 Float32Array hit buffer');
    if (bro.scene.sdfVulkanAvailable()) {
        assert(frame.usedVulkanGpu === true && bro.scene.sdfLastTraceUsedGpu() === true,
               'renderSdfToBuffer dispatched on hardware Vulkan queue');
    }
    // Center pixel (8, 8) must hit the sphere/box at z ≈ 1.2 (t ≈ 2.8)
    const centerBase = (8 * 16 + 8) * 8;
    assert(frame.hits[centerBase + 6] > 0.5, 'center pixel ray hits SDF surface');
    assert(frame.hits[centerBase + 0] > 2.0 && frame.hits[centerBase + 0] < 3.5,
           `center pixel hit distance t is accurate (${frame.hits[centerBase + 0]})`);
}
