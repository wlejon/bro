// three.js: N separate Mesh objects, six geometries x eight materials (two of
// them transparent, so both render-list sorts run), grouped ten to a Group,
// every transform animated. Bound by the CPU: scene-graph updates, culling,
// sorting and one draw call per visible mesh.

import { threeScene } from '../lib/three_runner.js';

threeScene({
    name: 'three_meshes',
    defaults: { n: 1000 },
    create({ THREE, n, rng, cfg }) {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x101018);
        const camera = new THREE.PerspectiveCamera(60, cfg.w / cfg.h, 0.1, 1000);
        scene.add(new THREE.AmbientLight(0x404050, 1.5));
        const sun = new THREE.DirectionalLight(0xffffff, 2.0);
        sun.position.set(5, 10, 7);
        scene.add(sun);

        const geometries = [
            new THREE.BoxGeometry(0.8, 0.8, 0.8),
            new THREE.SphereGeometry(0.5, 16, 12),
            new THREE.CylinderGeometry(0.35, 0.35, 0.9, 16),
            new THREE.TorusGeometry(0.4, 0.14, 10, 24),
            new THREE.ConeGeometry(0.45, 0.9, 16),
            new THREE.IcosahedronGeometry(0.5, 1),
        ];
        const col = () => new THREE.Color(rng.range(0.2, 1), rng.range(0.2, 1), rng.range(0.2, 1));
        const materials = [
            new THREE.MeshLambertMaterial({ color: col() }),
            new THREE.MeshPhongMaterial({ color: col(), shininess: 60 }),
            new THREE.MeshStandardMaterial({ color: col(), roughness: 0.4, metalness: 0.3 }),
            new THREE.MeshBasicMaterial({ color: col() }),
            new THREE.MeshLambertMaterial({ color: col() }),
            new THREE.MeshStandardMaterial({ color: col(), roughness: 0.8, metalness: 0.0 }),
            new THREE.MeshPhongMaterial({ color: col(), transparent: true, opacity: 0.6 }),
            new THREE.MeshStandardMaterial({ color: col(), transparent: true, opacity: 0.5 }),
        ];

        const side = Math.max(1, Math.ceil(Math.cbrt(n)));
        const spacing = 2.0;
        const extent = side * spacing;
        const groups = [];
        const meshes = [];
        let group = null;
        for (let i = 0; i < n; i++) {
            if (i % 10 === 0) {
                group = new THREE.Group();
                group.userData.spin = rng.range(-0.5, 0.5);
                groups.push(group);
                scene.add(group);
            }
            const m = new THREE.Mesh(rng.pick(geometries), rng.pick(materials));
            const gx = i % side, gy = Math.floor(i / side) % side, gz = Math.floor(i / (side * side));
            m.position.set((gx - side / 2) * spacing + rng.range(-0.3, 0.3), (gy - side / 2) * spacing + rng.range(-0.3, 0.3), (gz - side / 2) * spacing);
            m.userData.base = m.position.clone();
            m.userData.rx = rng.range(-2, 2);
            m.userData.ry = rng.range(-2, 2);
            m.userData.phase = rng.range(0, Math.PI * 2);
            group.add(m);
            meshes.push(m);
        }

        return {
            scene, camera,
            step(f, t) {
                for (let i = 0; i < groups.length; i++) {
                    groups[i].rotation.y = Math.sin(t * groups[i].userData.spin) * 0.05;
                }
                for (let i = 0; i < meshes.length; i++) {
                    const m = meshes[i], u = m.userData;
                    m.rotation.x = t * u.rx;
                    m.rotation.y = t * u.ry;
                    m.position.y = u.base.y + Math.sin(t * 2 + u.phase) * 0.25;
                    const s = 0.85 + 0.15 * Math.sin(t * 3 + u.phase);
                    m.scale.set(s, s, s);
                }
                const a = t * 0.25;
                const r = extent * 1.1 + 4;
                camera.position.set(Math.sin(a) * r, extent * 0.35, Math.cos(a) * r);
                camera.lookAt(0, 0, 0);
            },
        };
    },
});
