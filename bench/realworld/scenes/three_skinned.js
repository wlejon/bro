// three.js: N skinned characters built procedurally (no asset files): a
// 12-bone biped whose body is cylinders and a sphere merged into one skinned
// geometry, each character with its own skeleton and AnimationMixer blending
// a walk cycle with a wave. Bound by the mixer (keyframe interpolation,
// quaternion blending) and by skeleton.update (bone matrices).

import { threeScene } from '../lib/three_runner.js';

// [name, parent index, local position]
const BONES = [
    ['hips', -1, [0, 1.0, 0]],
    ['spine1', 0, [0, 0.25, 0]],
    ['spine2', 1, [0, 0.25, 0]],
    ['head', 2, [0, 0.3, 0]],
    ['lUpperArm', 2, [0.25, 0.2, 0]],
    ['lForearm', 4, [0, -0.3, 0]],
    ['rUpperArm', 2, [-0.25, 0.2, 0]],
    ['rForearm', 6, [0, -0.3, 0]],
    ['lThigh', 0, [0.12, 0, 0]],
    ['lShin', 8, [0, -0.45, 0]],
    ['rThigh', 0, [-0.12, 0, 0]],
    ['rShin', 10, [0, -0.45, 0]],
];

function buildGeometry(THREE) {
    // Each part: a geometry in bind space and its control points [bone, y]:
    // a vertex is weighted between the two control points around its height.
    const cyl = (r, y0, y1, x) => {
        const g = new THREE.CylinderGeometry(r, r, y0 - y1, 10, 8);
        g.translate(x, (y0 + y1) / 2, 0);
        return g;
    };
    const head = new THREE.SphereGeometry(0.15, 12, 8);
    head.translate(0, 1.95, 0);
    const parts = [
        [cyl(0.15, 1.75, 1.0, 0), [[0, 1.0], [1, 1.25], [2, 1.5]]],
        [head, [[3, 1.8]]],
        [cyl(0.06, 1.7, 1.1, 0.25), [[4, 1.7], [5, 1.4]]],
        [cyl(0.06, 1.7, 1.1, -0.25), [[6, 1.7], [7, 1.4]]],
        [cyl(0.08, 1.0, 0.1, 0.12), [[8, 1.0], [9, 0.55]]],
        [cyl(0.08, 1.0, 0.1, -0.12), [[10, 1.0], [11, 0.55]]],
    ];
    const pos = [], nrm = [], idx = [], si = [], sw = [];
    for (const [g, ctl] of parts) {
        const base = pos.length / 3;
        const p = g.attributes.position, nn = g.attributes.normal;
        for (let v = 0; v < p.count; v++) {
            const y = p.getY(v);
            pos.push(p.getX(v), y, p.getZ(v));
            nrm.push(nn.getX(v), nn.getY(v), nn.getZ(v));
            // control points are ordered by descending y for limbs, ascending for the torso
            let b0 = ctl[0][0], b1 = ctl[0][0], t = 0;
            if (ctl.length > 1) {
                const asc = ctl[1][1] > ctl[0][1];
                let k = 0;
                while (k < ctl.length - 2 && (asc ? y > ctl[k + 1][1] : y < ctl[k + 1][1])) k++;
                const ya = ctl[k][1], yb = ctl[k + 1][1];
                t = Math.min(1, Math.max(0, (y - ya) / (yb - ya)));
                b0 = ctl[k][0];
                b1 = ctl[k + 1][0];
            }
            si.push(b0, b1, 0, 0);
            sw.push(1 - t, t, 0, 0);
        }
        const gi = g.index;
        for (let i = 0; i < gi.count; i++) idx.push(gi.getX(i) + base);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    geo.setAttribute('normal', new THREE.Float32BufferAttribute(nrm, 3));
    geo.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(si, 4));
    geo.setAttribute('skinWeight', new THREE.Float32BufferAttribute(sw, 4));
    geo.setIndex(idx);
    return geo;
}

function buildClips(THREE) {
    const q = new THREE.Quaternion();
    const axis = { x: new THREE.Vector3(1, 0, 0), y: new THREE.Vector3(0, 1, 0), z: new THREE.Vector3(0, 0, 1) };
    const track = (bone, ax, times, angles) => {
        const values = [];
        for (const a of angles) { q.setFromAxisAngle(axis[ax], a); values.push(q.x, q.y, q.z, q.w); }
        return new THREE.QuaternionKeyframeTrack(bone + '.quaternion', times, values);
    };
    const T = [0, 0.25, 0.5, 0.75, 1.0];
    const walk = new THREE.AnimationClip('walk', 1.0, [
        track('lThigh', 'x', T, [0.5, 0, -0.5, 0, 0.5]),
        track('rThigh', 'x', T, [-0.5, 0, 0.5, 0, -0.5]),
        track('lShin', 'x', T, [0, 0.7, 0, 0.1, 0]),
        track('rShin', 'x', T, [0, 0.1, 0, 0.7, 0]),
        track('lUpperArm', 'x', T, [-0.4, 0, 0.4, 0, -0.4]),
        track('rUpperArm', 'x', T, [0.4, 0, -0.4, 0, 0.4]),
        track('lForearm', 'x', T, [-0.3, -0.5, -0.3, -0.5, -0.3]),
        track('rForearm', 'x', T, [-0.3, -0.5, -0.3, -0.5, -0.3]),
        track('spine2', 'y', T, [0.15, 0, -0.15, 0, 0.15]),
        new THREE.VectorKeyframeTrack('hips.position', T, [0, 1.0, 0, 0, 1.05, 0, 0, 1.0, 0, 0, 1.05, 0, 0, 1.0, 0]),
    ]);
    const W = [0, 0.4, 0.8, 1.2, 1.6];
    const wave = new THREE.AnimationClip('wave', 1.6, [
        track('rUpperArm', 'z', W, [0, -2.4, -2.0, -2.4, 0]),
        track('rForearm', 'z', W, [0, -0.6, 0.2, -0.6, 0]),
        track('head', 'z', W, [0, 0.2, -0.2, 0.2, 0]),
    ]);
    return { walk, wave };
}

threeScene({
    name: 'three_skinned',
    defaults: { n: 50 },
    create({ THREE, n, rng, cfg }) {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x15171c);
        const camera = new THREE.PerspectiveCamera(50, cfg.w / cfg.h, 0.1, 1000);
        scene.add(new THREE.HemisphereLight(0xc0d0ff, 0x302010, 1.2));
        const sun = new THREE.DirectionalLight(0xffffff, 2.0);
        sun.position.set(3, 8, 5);
        scene.add(sun);

        const geometry = buildGeometry(THREE);
        const { walk, wave } = buildClips(THREE);
        const material = new THREE.MeshStandardMaterial({ color: 0xb0a090, roughness: 0.6 });
        const side = Math.max(1, Math.ceil(Math.sqrt(n)));
        const spacing = 1.4;
        const mixers = [];
        for (let i = 0; i < n; i++) {
            const bones = [];
            for (const [name, parent, p] of BONES) {
                const b = new THREE.Bone();
                b.name = name;
                b.position.set(p[0], p[1], p[2]);
                if (parent >= 0) bones[parent].add(b);
                bones.push(b);
            }
            const mesh = new THREE.SkinnedMesh(geometry, material);
            mesh.add(bones[0]);
            mesh.bind(new THREE.Skeleton(bones));
            const gx = i % side, gz = Math.floor(i / side);
            mesh.position.set((gx - (side - 1) / 2) * spacing, 0, (gz - (side - 1) / 2) * spacing);
            mesh.rotation.y = rng.range(-0.6, 0.6);
            scene.add(mesh);
            const mixer = new THREE.AnimationMixer(mesh);
            const a = mixer.clipAction(walk);
            a.timeScale = rng.range(0.8, 1.2);
            a.time = rng.range(0, 1);
            a.play();
            const b = mixer.clipAction(wave);
            b.setEffectiveWeight(rng.range(0.3, 0.8));
            b.time = rng.range(0, 1.6);
            b.play();
            mixers.push(mixer);
        }
        const extent = side * spacing;
        return {
            scene, camera,
            step(f, t) {
                for (let i = 0; i < mixers.length; i++) mixers[i].update(1 / 60);
                const a = t * 0.3;
                const r = extent * 0.9 + 3;
                camera.position.set(Math.sin(a) * r, 1.5 + extent * 0.35, Math.cos(a) * r);
                camera.lookAt(0, 0.9, 0);
            },
        };
    },
});
