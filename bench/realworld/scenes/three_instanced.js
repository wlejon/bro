// three.js: one InstancedMesh of N instances, every instance matrix rewritten
// each frame through Object3D.updateMatrix + setMatrixAt (CPU math plus one
// N*64-byte buffer upload, one draw call). Per-instance colours set once.

import { threeScene } from '../lib/three_runner.js';

threeScene({
    name: 'three_instanced',
    defaults: { n: 10000 },
    create({ THREE, n, rng, cfg }) {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x0c0e14);
        const camera = new THREE.PerspectiveCamera(60, cfg.w / cfg.h, 0.1, 2000);
        scene.add(new THREE.AmbientLight(0x404050, 1.2));
        const sun = new THREE.DirectionalLight(0xffffff, 1.8);
        sun.position.set(5, 10, 7);
        scene.add(sun);

        const geometry = new THREE.IcosahedronGeometry(0.35, 1);
        const material = new THREE.MeshStandardMaterial({ roughness: 0.45, metalness: 0.15 });
        const mesh = new THREE.InstancedMesh(geometry, material, n);
        mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
        // Instances move every frame; a bounding sphere computed once would cull wrongly.
        mesh.frustumCulled = false;
        scene.add(mesh);

        const side = Math.max(1, Math.ceil(Math.cbrt(n)));
        const spacing = 1.2;
        const extent = side * spacing;
        const base = new Float32Array(n * 3);
        const spin = new Float32Array(n * 3);
        const color = new THREE.Color();
        for (let i = 0; i < n; i++) {
            const gx = i % side, gy = Math.floor(i / side) % side, gz = Math.floor(i / (side * side));
            base[i * 3] = (gx - side / 2) * spacing;
            base[i * 3 + 1] = (gy - side / 2) * spacing;
            base[i * 3 + 2] = (gz - side / 2) * spacing;
            spin[i * 3] = rng.range(-2, 2);
            spin[i * 3 + 1] = rng.range(-2, 2);
            spin[i * 3 + 2] = rng.range(0, Math.PI * 2);
            color.setHSL(rng(), 0.6, 0.55);
            mesh.setColorAt(i, color);
        }
        const dummy = new THREE.Object3D();

        return {
            scene, camera,
            step(f, t) {
                for (let i = 0; i < n; i++) {
                    const ph = spin[i * 3 + 2];
                    dummy.position.set(base[i * 3] + Math.sin(t + ph) * 0.2, base[i * 3 + 1] + Math.cos(t * 1.3 + ph) * 0.2, base[i * 3 + 2]);
                    dummy.rotation.set(t * spin[i * 3], t * spin[i * 3 + 1], 0);
                    const s = 0.8 + 0.2 * Math.sin(t * 2 + ph);
                    dummy.scale.set(s, s, s);
                    dummy.updateMatrix();
                    mesh.setMatrixAt(i, dummy.matrix);
                }
                mesh.instanceMatrix.needsUpdate = true;
                const a = t * 0.2;
                const r = extent * 1.2 + 3;
                camera.position.set(Math.sin(a) * r, extent * 0.3, Math.cos(a) * r);
                camera.lookAt(0, 0, 0);
            },
        };
    },
});
