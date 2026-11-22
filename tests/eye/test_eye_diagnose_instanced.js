// Tests bro.eye.diagnose coverage over InstancedMeshNode (opaque, emissive,
// tint_mismatch, custom shader exemption, and instanced id_translucent + translucent_striation).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '320');
canvas.setAttribute('height', '240');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setAmbient(0.35, 0.35, 0.38);
    scene.createLight({
        type: 'directional',
        direction: [-0.5, -0.7, -0.5],
        color: [1, 1, 1],
        intensity: 2.5,
    });
    scene.setCamera({
        fov: 55, near: 0.1, far: 100,
        position: [0, 1.8, 5.5],
        target: [0, 0.8, 0],
        up: [0, 1, 0],
    });

    function makeInstBuf(positions) {
        const arr = [];
        for (const [x, y, z] of positions) {
            arr.push(
                1, 0, 0, x,
                0, 1, 0, y,
                0, 0, 1, z,
                1, 1, 1, 1
            );
        }
        return new Float32Array(arr);
    }

    // 1. Untextured opaque InstancedMeshNode with castsShadow: false -> flat_plastic + no_shadow_caster
    const boxMesh = Mesh.box(0.35, 0.5, 0.35);
    const flatInst = scene.createInstancedMesh({
        name: 'FlatInstBoxes',
        mesh: boxMesh,
        color: [0.6, 0.62, 0.64, 1.0],
        castsShadow: false,
    });
    flatInst.setInstances(makeInstBuf([
        [-1.8, 0.5, 0.0],
        [-1.8, 0.5, 1.2],
        [-1.8, 0.5, -1.2],
    ]));

    // 2. Emissive InstancedMeshNode -> must be exempted from bright_outlier / bloom_hotspot / flat_plastic
    const glowInst = scene.createInstancedMesh({
        name: 'EmissiveInstLights',
        mesh: Mesh.box(0.28, 0.28, 0.28),
        color: [1.0, 0.92, 0.75, 1.0],
        emissive: 2.5,
        castsShadow: false,
    });
    glowInst.setInstances(makeInstBuf([
        [-0.6, 1.2, 0.5],
        [-0.6, 0.5, 0.5],
    ]));

    // 3. InstancedMeshNode with leafCard vertex colors + cyan texture -> tint_mismatch
    const texW = 16, texH = 16;
    const cyanTex = new Uint8Array(texW * texH * 4);
    const nrmTex = new Uint8Array(texW * texH * 4);
    for (let i = 0; i < texW * texH; ++i) {
        cyanTex[i * 4 + 0] = 200; cyanTex[i * 4 + 1] = 235; cyanTex[i * 4 + 2] = 250; cyanTex[i * 4 + 3] = 255;
        nrmTex[i * 4 + 0] = 128 + ((i % 2) * 24 - 12); nrmTex[i * 4 + 1] = 128; nrmTex[i * 4 + 2] = 255; nrmTex[i * 4 + 3] = 255;
    }
    const tintInst = scene.createInstancedMesh({
        name: 'InstRedTintedWing',
        mesh: Mesh.leafCard('oval', { width: 0.9, length: 0.9, fullUV: true }),
        texture: { width: texW, height: texH, data: cyanTex },
        normalTexture: { width: texW, height: texH, data: nrmTex },
        color: [1, 1, 1, 1],
        doubleSided: true,
        vertexColorTint: true,
    });
    tintInst.setInstances(makeInstBuf([
        [0.4, 0.85, 1.5],
    ]));

    // 4. Custom-shader procedural instanced facade (no CPU textures, high procedural detail -> no false flat_plastic)
    const facadeShader = `
        module hook;
        import mesh;
        pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
            var out = s;
            let checker = ((floor(v.world_pos.x * 8.0) + floor(v.world_pos.y * 8.0)) % 2.0);
            out.base_color = mix(vec3<f32>(0.22, 0.25, 0.30), vec3<f32>(0.68, 0.72, 0.76), abs(checker));
            out.normal = normalize(v.normal + vec3<f32>(sin(v.world_pos.x * 16.0) * 0.25, cos(v.world_pos.y * 16.0) * 0.25, 0.0));
            return out;
        }
    `;
    const customInst = scene.createInstancedMesh({
        name: 'ProceduralFacadeInst',
        mesh: Mesh.box(0.5, 0.7, 0.2),
        color: [1, 1, 1, 1],
        module: facadeShader,
        file: 'facade_diag_test.bk',
    });
    customInst.setInstances(makeInstBuf([
        [1.7, 0.7, 0.6],
    ]));

    // 4b. Opaque InstancedMeshNode populated via setInstancesFromTransforms (rec[15]=(variant+0.5)/256 < 0.99)
    //     Must render & be diagnosed as opaque (translucent=false) since SceneMeshDrawer uses node.color[3]
    const xformInst = scene.createInstancedMesh({
        name: 'OpaqueTransformsInst',
        mesh: Mesh.box(0.25, 0.25, 0.25),
        color: [0.85, 0.82, 0.78, 1.0],
    });
    xformInst.setInstancesFromTransforms(new Float32Array([
        1.1, 0.3, 1.8,  0, 0, 0, 1,  1.0, 0,
    ]));

    flush();
    const dgOpaque = bro.eye.diagnose(scene, { minPixels: 16 });
    assert(dgOpaque.ok === true, 'diagnose ok on instanced opaque scene');
    const flatMeshStats = dgOpaque.meshes.find(m => m.name === 'FlatInstBoxes');
    assert(flatMeshStats && flatMeshStats.instanced === true && flatMeshStats.instanceCount === 3,
        'diagnose reports instanced=true and instanceCount=3 on FlatInstBoxes');
    const xformStats = dgOpaque.meshes.find(m => m.name === 'OpaqueTransformsInst');
    assert(xformStats && xformStats.translucent === false,
        'setInstancesFromTransforms with color.a=1.0 is diagnosed as opaque (rec[15] is atlas index)');
    assert(dgOpaque.anomalies.some(a => a.kind === 'flat_plastic' && a.nodeName === 'FlatInstBoxes'),
        'diagnose catches flat_plastic on untextured InstancedMeshNode');
    assert(dgOpaque.anomalies.some(a => a.kind === 'no_shadow_caster' && a.nodeName === 'FlatInstBoxes'),
        'diagnose catches no_shadow_caster on opaque InstancedMeshNode');
    assert(!dgOpaque.anomalies.some(a => (a.kind === 'bright_outlier' || a.kind === 'bloom_hotspot') && a.nodeName === 'EmissiveInstLights'),
        'diagnose exempts emissive InstancedMeshNode from bright_outlier/bloom_hotspot');
    assert(dgOpaque.anomalies.some(a => a.kind === 'tint_mismatch' && a.nodeName === 'InstRedTintedWing'),
        'diagnose catches tint_mismatch on InstancedMeshNode');
    assert(!dgOpaque.anomalies.some(a => a.kind === 'flat_plastic' && a.nodeName === 'ProceduralFacadeInst'),
        'diagnose does not false-flag custom-shader procedural InstancedMeshNode as flat_plastic');

    // 5. InstancedMeshNode translucent ONLY (NO translucent MeshNode in scene!) -> id_translucent + translucent_striation
    const transCone = scene.createInstancedMesh({
        name: 'InstCoarseGlazing',
        mesh: Mesh.cone(0.55, 1.4, 16, 4, false),
        color: [0.6, 0.85, 1.0, 0.35],
        doubleSided: true,
        castsShadow: false,
    });
    transCone.setInstances(makeInstBuf([
        [0.0, 1.0, 2.2],
    ]));
    flush();

    const dgTrans = bro.eye.diagnose(scene, { minPixels: 16 });
    const transStats = dgTrans.meshes.find(m => m.name === 'InstCoarseGlazing' && m.translucent === true);
    assert(transStats && transStats.instanced === true && transStats.instanceCount === 1,
        'diagnose runs id_translucent and attributes instanced translucent mesh');
    assert(dgTrans.anomalies.some(a => a.kind === 'translucent_striation' && a.nodeName === 'InstCoarseGlazing'),
        'diagnose flags translucent_striation on coarse doubleSided InstancedMeshNode');

    console.log('OK test_eye_diagnose_instanced: meshes=' + dgTrans.meshes.length + ' anomalies=' + dgTrans.anomalies.length);
}
