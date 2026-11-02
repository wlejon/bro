// three.js: a MeshStandardMaterial scene of `meshes` objects (default 100) on
// a ground plane lit by N moving point lights; shadows=1 adds a shadow-casting
// directional light (PCF) with every mesh casting and the ground receiving.
// Bound by the fragment shader: the light loop runs per pixel for every light.

import { threeScene } from '../lib/three_runner.js';

threeScene({
    name: 'three_lights',
    defaults: { n: 8, meshes: 100 },
    create({ THREE, n, rng, cfg }) {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x080810);
        const camera = new THREE.PerspectiveCamera(55, cfg.w / cfg.h, 0.1, 500);
        scene.add(new THREE.AmbientLight(0x202030, 0.6));

        const ground = new THREE.Mesh(new THREE.PlaneGeometry(60, 60), new THREE.MeshStandardMaterial({ color: 0x606060, roughness: 0.9 }));
        ground.rotation.x = -Math.PI / 2;
        ground.receiveShadow = !!cfg.shadows;
        scene.add(ground);

        if (cfg.shadows) {
            const sun = new THREE.DirectionalLight(0xffffff, 1.2);
            sun.position.set(10, 20, 8);
            sun.castShadow = true;
            sun.shadow.mapSize.set(2048, 2048);
            const c = sun.shadow.camera;
            c.left = -25; c.right = 25; c.top = 25; c.bottom = -25; c.near = 1; c.far = 60;
            scene.add(sun);
        }

        const geos = [new THREE.SphereGeometry(0.7, 32, 16), new THREE.BoxGeometry(1.1, 1.1, 1.1), new THREE.TorusKnotGeometry(0.5, 0.18, 96, 12)];
        const meshes = [];
        const count = cfg.meshes;
        const side = Math.ceil(Math.sqrt(count));
        for (let i = 0; i < count; i++) {
            const mat = new THREE.MeshStandardMaterial({ color: new THREE.Color().setHSL(rng(), 0.5, 0.6), roughness: rng.range(0.15, 0.9), metalness: rng.range(0, 0.8) });
            const m = new THREE.Mesh(geos[i % 3], mat);
            m.position.set(((i % side) - (side - 1) / 2) * 2.6, 0.9, (Math.floor(i / side) - (side - 1) / 2) * 2.6);
            m.castShadow = !!cfg.shadows;
            m.receiveShadow = !!cfg.shadows;
            m.userData.spin = rng.range(-1.5, 1.5);
            scene.add(m);
            meshes.push(m);
        }

        const lights = [];
        for (let i = 0; i < n; i++) {
            const l = new THREE.PointLight(new THREE.Color().setHSL(i / Math.max(1, n), 0.9, 0.55), 40, 18, 2);
            l.userData.r = rng.range(3, side * 1.4);
            l.userData.speed = rng.range(0.2, 0.8) * (i % 2 ? 1 : -1);
            l.userData.phase = rng.range(0, Math.PI * 2);
            l.userData.h = rng.range(1.5, 4);
            scene.add(l);
            lights.push(l);
        }
        const extent = side * 2.6;
        return {
            scene, camera,
            step(f, t) {
                for (const l of lights) {
                    const u = l.userData;
                    const a = u.phase + t * u.speed;
                    l.position.set(Math.cos(a) * u.r, u.h + Math.sin(t * 1.7 + u.phase), Math.sin(a) * u.r);
                }
                for (const m of meshes) { m.rotation.y = t * m.userData.spin; m.rotation.x = t * 0.3; }
                const a = t * 0.15;
                camera.position.set(Math.sin(a) * extent * 0.8, extent * 0.55, Math.cos(a) * extent * 0.8);
                camera.lookAt(0, 0, 0);
            },
        };
    },
});
