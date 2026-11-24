// Headless integration test for Milestone 1 Zero-Copy GPU Fabric:
//   1. Automatic SPIR-V push-constant parameter spilling (>128B / >256B BDA struct)
//   2. Table.packToKernelBuffer (AoS + SoA, target reuse) & unpackFromKernelBuffer + ReactiveQuery chunk reuse
//   3. Zero-copy bro.kernel.fromTensor / toTensor + mixed-device guard
//   4. Table.packToKernelBuffer -> single canonical InstancedMeshNode GPU shader + cull spheres path

const q = bro.table;
assert(typeof q === 'object', 'bro.table exists');

// ----------------------------------------------------------------------------
// 1. Automatic SPIR-V push-constant spilling (>128B parameter list)
// ----------------------------------------------------------------------------
if (bro.kernel && bro.kernel.available) {
    const scalarParams = [];
    const sumTerms = [];
    let expectedSum = 0.0;
    for (let i = 0; i < 40; i++) {
        scalarParams.push(`p${i}: f32`);
        sumTerms.push(`p${i}`);
        expectedSum += (i + 1) * 0.5;
    }
    const spillSrc = `
        @kernel @grid(1)
        fn k_many_params(g: grid<1>, out: rw_store<f32, 1>, ${scalarParams.join(', ')}) {
            let i = g.x();
            out[i] = ${sumTerms.join(' + ')};
        }
    `;
    const spillMod = bro.kernel.compile(spillSrc, { file: 'spill_params.bk' });
    const kSpill = spillMod.kernel('k_many_params');
    const residency = bro.kernel.gpu ? 'gpu' : 'host';
    const outKb = bro.kernel.createBuffer(8 * 4, { residency });
    const args = [outKb];
    for (let i = 0; i < 40; i++) args.push((i + 1) * 0.5);

    if (bro.kernel.gpu) kSpill.dispatch(8, ...args);
    else kSpill.run(8, ...args);

    const got = outKb.read(new Float32Array(8));
    for (let i = 0; i < 8; i++) {
        assert(Math.abs(got[i] - expectedSum) < 1e-2, `spilled param kernel out[${i}]=${got[i]} want ${expectedSum}`);
    }
}

// ----------------------------------------------------------------------------
// 2. packToKernelBuffer (AoS + SoA + target reuse) & unpackFromKernelBuffer
// ----------------------------------------------------------------------------
{
    const tbl = q.create({
        id: 'i32',
        x: 'f32',
        y: 'f32',
        z: 'f32',
    });
    const N = 9000; // 3 chunks (4096, 4096, 808)
    const batch = [];
    for (let i = 0; i < N; i++) {
        batch.push({ id: i, x: i * 1.0, y: i * 2.0, z: i * 0.5 });
    }
    tbl.insertMany(batch);
    assert(tbl.chunkCount === 3, '3 chunks created');

    const residency = (bro.kernel && bro.kernel.gpu) ? 'gpu' : 'host';
    const aosKb = tbl.packToKernelBuffer(['x', 'y', 'z'], {
        layout: 'aos',
        dtype: 'f32',
        strideFloats: 4,
        residency,
    });
    assert(aosKb.byteLength === N * 16, 'AoS strideFloats=4 byteLength matches');
    const aosRead = aosKb.read(new Float32Array(12));
    assert(Math.abs(aosRead[4] - 1.0) < 1e-5 && Math.abs(aosRead[5] - 2.0) < 1e-5 &&
           Math.abs(aosRead[6] - 0.5) < 1e-5 && aosRead[7] === 0.0, 'AoS packing + zero-padding verified');

    // Reuse existing target buffer without reallocation
    const reused = tbl.packToKernelBuffer(['x', 'y'], {
        layout: 'soa',
        dtype: 'f32',
        residency,
        target: aosKb,
    });
    assert(reused === aosKb, 'packToKernelBuffer with target returns the same KernelBuffer');

    const soaKb = tbl.packToKernelBuffer(['x', 'y'], { layout: 'soa', dtype: 'f32', residency });
    assert(soaKb.byteLength === N * 2 * 4, 'SoA byteLength matches');

    if (bro.kernel && bro.kernel.available) {
        const mod = bro.kernel.compile(`
            @kernel @grid(1)
            fn k_scale_soa(g: grid<1>, buf: rw_store<f32, 1>, n: i32) {
                let i = g.x();
                if (i < n) {
                    buf[i] = buf[i] + 100.0;
                    buf[n + i] = buf[n + i] * 3.0;
                }
            }
        `, { file: 'soa_scale.bk' });
        const kScale = mod.kernel('k_scale_soa');
        if (bro.kernel.gpu) kScale.dispatch(N, soaKb, N);
        else kScale.run(N, soaKb, N);
    } else {
        const tmp = soaKb.read(new Float32Array(N * 2));
        for (let i = 0; i < N; i++) { tmp[i] += 100.0; tmp[N + i] *= 3.0; }
        soaKb.write(tmp);
    }

    // Attach ReactiveQuery and unpack ONLY chunk 1 (rowOffset 4200, rowCount 100)
    const rq = tbl.query().where(q.ge('id', 0)).reactive();
    const events = [];
    rq.onChange((ev) => events.push(ev));

    const unpacked = tbl.unpackFromKernelBuffer(['x', 'y'], soaKb, {
        layout: 'soa',
        totalRows: N,
        rowOffset: 4200,
        bufferRowOffset: 4200,
        rowCount: 100,
    });
    assert(unpacked === 100, `unpacked=${unpacked} expected 100`);
    assert(rq.dirty() === true, 'ReactiveQuery marked dirty after partial unpack');
    const rr = rq.refresh();
    assert(events.length === 1, 'ReactiveQuery fired change event');
    assert(rr.recomputedChunks === 1 && rr.reusedChunks === 2,
           `expected recomputed=1 reused=2 got ${rr.recomputedChunks}/${rr.reusedChunks}`);

    assert(Math.abs(tbl.get(10, 'x') - 10.0) < 1e-4, 'chunk 0 row remained untouched');
    assert(Math.abs(tbl.get(4205, 'x') - (4205.0 + 100.0)) < 1e-2 &&
           Math.abs(tbl.get(4205, 'y') - (4205.0 * 6.0)) < 1e-1,
           'chunk 1 row updated from KernelBuffer');
    rq.dispose();
}

// ----------------------------------------------------------------------------
// 3. Zero-copy bro.kernel.fromTensor & bro.kernel.toTensor
// ----------------------------------------------------------------------------
if (bro.kernel && bro.kernel.available && bro.tensor && bro.tensor.available !== false) {
    bro.tensor.init();
    const mod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_add_one(g: grid<1>, a: rw_store<f32, 1>) {
            let i = g.x();
            a[i] = a[i] + 10.0;
        }
        @kernel @grid(1)
        fn k_copy2(g: grid<1>, dst: rw_store<f32, 1>, src: store<f32, 1>) {
            let i = g.x();
            dst[i] = src[i] + 1.0;
        }
    `, { file: 'tensor_zc.bk' });
    const kAdd = mod.kernel('k_add_one');
    const kCopy2 = mod.kernel('k_copy2');

    // Explicit CPU tensor via bro.kernel.toTensor tests true zero-copy Host wrap (no copyToTensor needed);
    // Vulkan/default tensor tests Vulkan BDA zero-copy or CUDA staging fallback.
    const seedKb = bro.kernel.createBuffer(8 * 4, { residency: 'host' });
    seedKb.write(new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]));
    const cpuT = bro.kernel.toTensor(seedKb, [4, 2], { dtype: 'f32', device: 'cpu' });
    const cpuZc = bro.kernel.fromTensor(cpuT);
    assert(cpuZc.residency === 'host', 'CPU tensor wraps as host zero-copy KernelBuffer');
    kAdd.run(8, cpuZc);
    const directCpu = cpuT.download();
    for (let i = 0; i < 8; i++) {
        assert(Math.abs(directCpu[i] - (i + 1 + 10.0)) < 1e-4, `zero-copy CPU fromTensor[${i}]=${directCpu[i]}`);
    }

    const t = bro.tensor.createTensor(4, 2);
    t.upload(new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]));
    const zcBuf = bro.kernel.fromTensor(t);

    if (bro.kernel.gpu && zcBuf.residency === 'gpu' && thrownGuardNeeded(t)) {
        kAdd.dispatch(8, zcBuf);
        const directOut = t.download();
        for (let i = 0; i < 8; i++) {
            assert(Math.abs(directOut[i] - (i + 1 + 10.0)) < 1e-4, `zero-copy VK fromTensor[${i}]=${directOut[i]}`);
        }
    } else if (!bro.kernel.gpu && zcBuf.residency === 'host') {
        kAdd.run(8, zcBuf);
        const directOut = t.download();
        for (let i = 0; i < 8; i++) {
            assert(Math.abs(directOut[i] - (i + 1 + 10.0)) < 1e-4, `zero-copy fromTensor[${i}]=${directOut[i]}`);
        }
    }

    const roundTrip = bro.kernel.toTensor(cpuZc, [4, 2], { dtype: 'f32', device: 'cpu' });
    const rtVals = roundTrip.download();
    for (let i = 0; i < 8; i++) {
        assert(Math.abs(rtVals[i] - directCpu[i]) < 1e-4, `toTensor[${i}] matches`);
    }

    if (bro.kernel.gpu) {
        const engBuf = bro.kernel.createBuffer(8 * 4, { residency: 'gpu' });
        let threw = false;
        try {
            kCopy2.dispatch(8, engBuf, zcBuf);
        } catch (e) {
            threw = String(e.message || e).includes('copy: true');
        }
        if (thrownGuardNeeded(t)) {
            assert(threw, 'mixing engine buffer with zero-copy Vulkan tensor throws clear error');
        }
        const stagedBuf = bro.kernel.fromTensor(t, { copy: true, residency: 'gpu' });
        kCopy2.dispatch(8, engBuf, stagedBuf);
        const copied = engBuf.read(new Float32Array(8));
        const tOut = t.download();
        for (let i = 0; i < 8; i++) {
            assert(Math.abs(copied[i] - (tOut[i] + 1.0)) < 1e-4, `staged copy:true[${i}]`);
        }
    }
}

function thrownGuardNeeded(t) {
    try { return t.device === 'vulkan'; } catch (_) { return false; }
}

// ----------------------------------------------------------------------------
// 4. Table.packToKernelBuffer -> InstancedMeshNode vertex hook + GPU cull spheres
// ----------------------------------------------------------------------------
{
    const canvas = document.createElement('canvas');
    canvas.setAttribute('width', '64');
    canvas.setAttribute('height', '64');
    document.body.appendChild(canvas);
    flush();

    const scene = canvas.getContext('scene');
    if (scene && bro.kernel && bro.kernel.gpu) {
        scene.setToneMap({ mode: 'linear', gamma: 1.0 });
        scene.setAmbient({ color: [0, 0, 0] });
        scene.createLight({ type: 'directional', intensity: 0 });
        scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 5], target: [0, 0, 0], up: [0, 1, 0] });

        const itbl = q.create({ px: 'f32', py: 'f32', pz: 'f32', s: 'f32', cr: 'f32', cg: 'f32', cb: 'f32', ca: 'f32' });
        itbl.insertMany([{ px: 0.0, py: 0.0, pz: 0.0, s: 2.0, cr: 0.0, cg: 1.0, cb: 0.0, ca: 1.0 }]);

        // Pack transform spheres [x, y, z, radius] and colors [r, g, b, a] via the single packToKernelBuffer API.
        const poseKb = itbl.packToKernelBuffer(['px', 'py', 'pz', 's'], { layout: 'aos', dtype: 'f32', residency: 'gpu' });
        const colorKb = itbl.packToKernelBuffer(['cr', 'cg', 'cb', 'ca'], { layout: 'aos', dtype: 'f32', residency: 'gpu' });
        assert(poseKb.byteLength === 16 && colorKb.byteLength === 16, 'packToKernelBuffer AoS vec4 buffers ready');

        const hook = `
            module inst_hook;
            import mesh;
            @binding(4, 14) var poses: read_buffer<vec4<f32>>;
            @binding(4, 15) var tints: read_buffer<vec4<f32>>;
            pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                var out = v;
                let p = poses[v.instance_index];
                out.pos = out.pos * p.w + p.xyz;
                out.v0 = tints[v.instance_index];
                return out;
            }
            pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                var out = s;
                out.base_color = vec3<f32>(0.0);
                out.emissive = v.v0.rgb;
                return out;
            }
        `;

        const idRow = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1]);
        const node = scene.createInstancedMesh({ mesh: Mesh.box(), color: [1, 1, 1, 1], instances: idRow });
        assert(typeof node.setInstanceBuffer === 'undefined', 'setInstanceBuffer removed under single-path rule');
        node.setShader({ module: hook });
        node.setShaderBuffer('poses', poseKb);
        node.setShaderBuffer('tints', colorKb);
        node.setInstanceCullSpheres(poseKb);
        node.setCullBounds([-2, -2, -2], [2, 2, 2]);

        const centerPx = () => {
            const img = scene.captureFrame();
            const i = (32 * img.width + 32) * 4;
            return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2], a: img.data[i + 3] };
        };

        const pGreen = centerPx();
        assert(pGreen.a > 0 && pGreen.g > 40 && pGreen.g > 3 * pGreen.r && pGreen.g > 3 * pGreen.b,
               `canonical GPU hook path rendered green: rgba(${pGreen.r},${pGreen.g},${pGreen.b},${pGreen.a})`);

        // Mutate the bound GPU color buffer in-place via a compute kernel -> turns red!
        const colorMod = bro.kernel.compile(`
            @kernel @grid(1)
            fn k_recolor_inst(g: grid<1>, tints: rw_store<f32, 1>) {
                let i = g.x();
                let base = i * 4;
                tints[base + 0] = 1.0;
                tints[base + 1] = 0.0;
                tints[base + 2] = 0.0;
                tints[base + 3] = 1.0;
            }
        `, { file: 'recolor_inst.bk' });
        colorMod.kernel('k_recolor_inst').dispatch(1, colorKb);

        const pRed = centerPx();
        assert(pRed.a > 0 && pRed.r > 40 && pRed.r > 3 * pRed.g && pRed.r > 3 * pRed.b,
               `in-place GPU kernel update rendered red: rgba(${pRed.r},${pRed.g},${pRed.b},${pRed.a})`);
    }
}

console.log('test_table_gpu_fabric: OK');
