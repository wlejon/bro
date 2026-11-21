// Stacked additive + subtractive SVDAG world layer via `bro.voxeldag`:
//   - creates a VoxelWorld on a SceneGraph and verifies `node instanceof SceneNode`;
//   - batch pointer instances share brick/attribute memory (node count stays tiny);
//   - animated clip frames flip rendered colors at zero pool generation cost;
//   - `buffers()` feeds `voxeldag::is_carved_at` inside a terrain mesh hook to
//     discard the heightfield across a carved pit and expose 3D voxel geometry.

function makeScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 45, near: 0.2, far: 200, position: [0, 4.5, 6.2], target: [0, -0.4, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0.45, 0.45, 0.45] });
        sc.createLight({ type: 'directional', direction: [-0.2, -1.0, -0.4], intensity: 2.4 });
    }
    return { canvas: cv, scene: sc };
}

function makePlane(halfExtent, segs) {
    const n = segs + 1;
    const positions = new Float32Array(n * n * 3);
    const normals = new Float32Array(n * n * 3);
    for (let z = 0; z < n; z++) {
        const vz = -halfExtent + 2 * halfExtent * z / segs;
        for (let x = 0; x < n; x++) {
            const vx = -halfExtent + 2 * halfExtent * x / segs;
            const i = (z * n + x) * 3;
            positions[i] = vx;
            positions[i + 1] = 0;
            positions[i + 2] = vz;
            normals[i] = 0;
            normals[i + 1] = 1;
            normals[i + 2] = 0;
        }
    }
    const indices = new Uint32Array(segs * segs * 6);
    let k = 0;
    for (let z = 0; z < segs; z++) {
        for (let x = 0; x < segs; x++) {
            const a = z * n + x, b = a + 1, c = a + n, d = c + 1;
            indices[k++] = a; indices[k++] = c; indices[k++] = b;
            indices[k++] = b; indices[k++] = c; indices[k++] = d;
        }
    }
    return { positions, normals, indices };
}

const probe = makeScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else if (!bro.kernel.available || !bro.kernel.gpu || !bro.voxeldag) {
    document.body.removeChild(probe.canvas);
    flush();
    skipTest('no GPU / bro.voxeldag runtime');
} else {
    document.body.removeChild(probe.canvas);
    flush();

    assert(typeof bro.voxeldag.module === 'string' && bro.voxeldag.module.indexOf('is_carved_at') >= 0,
           'bro.voxeldag.module exposes voxeldag.bk');

    // 1. Batch pointer-instancing + O(1) animation clip verification
    {
        const s = makeScene(128);
        s.scene.setCamera({ fov: 45, near: 0.2, far: 150, position: [0, 0, 11], target: [0, 0, 0] });
        const world = bro.voxeldag.createWorld(s.scene);
        assert(world.node instanceof SceneNode, 'world.node is a live SceneNode');

        const red = world.addArchetypeFromBox([0.95, 0.1, 0.1, 1.0], 1);
        const blue = world.addArchetypeFromBox([0.1, 0.15, 0.95, 1.0], 1);
        const clip = world.registerAnimClip([red, blue]);
        const rid = world.addRegion({ voxelSize: 0.25, maxDepth: 2, center: [0, 0, 0] });

        const placements = [];
        for (let oz = 0; oz < 2; oz++) {
            for (let oy = 0; oy < 2; oy++) {
                for (let ox = 0; ox < 2; ox++) {
                    placements.push({ ix: ox, iy: oy, iz: oz, archetype: red });
                }
            }
        }
        world.placeBatch(rid, placements, 1);
        const stAfterBatch = world.stats();
        for (let oz = 0; oz < 2; oz++) {
            for (let oy = 0; oy < 2; oy++) {
                for (let ox = 0; ox < 2; ox++) {
                    world.placeAnimated(rid, ox, oy, oz, clip, 0, false, 1);
                }
            }
        }

        world.setAutoAnimate(false);
        world.setShadowSteps(0);
        world.setAnimTick(0);
        const gen0 = world.stats().generation;
        const img0 = s.scene.captureFrame();
        const mid = (64 * 128 + 64) * 4;
        assert(img0.data[mid] > img0.data[mid + 2] + 50,
               `tick 0 renders red frame (${img0.data[mid]},${img0.data[mid + 1]},${img0.data[mid + 2]})`);

        world.setAnimTick(1);
        assert(world.stats().generation === gen0, 'setAnimTick does not bump DAG generation');
        assert(stAfterBatch.bricks > 0 && stAfterBatch.nodes > 0, 'pool stats report nodes & bricks');
        const img1 = s.scene.captureFrame();
        assert(img1.data[mid + 2] > img1.data[mid] + 50,
               `tick 1 renders blue frame (${img1.data[mid]},${img1.data[mid + 1]},${img1.data[mid + 2]})`);

        document.body.removeChild(s.canvas);
        flush();
    }

    // 2. Stacked Carve + Solid cave + heightfield discard_fragment() + depth interop
    {
        const s = makeScene(192);
        const world = bro.voxeldag.createWorld(s.scene);
        world.setShadowSteps(0);

        const rid = world.addRegion({ voxelSize: 0.25, maxDepth: 3, center: [0, 0, 0] });
        const carveBox = world.addArchetypeFromBox([1, 1, 1, 1], 1, { state: 'carve' });
        const warmFloor = world.addArchetypeFromBox([0.96, 0.42, 0.08, 1.0], 1, { roughness: 0.8 });
        const glowCube = world.addArchetypeFromBox([0.98, 0.52, 0.12, 1.0], 1, { roughness: 0.7, emissive: 0.6 });
        const merged = world.mergeArchetypes(carveBox, warmFloor);
        assert(typeof merged.ref === 'number' && merged.ref >= 0, 'mergeArchetypes returns combined ref');
        const clip = world.registerAnimClip([glowCube, warmFloor]);
        const animRef = world.makeAnimRef(clip, 1);
        assert(typeof animRef.ref === 'number' && animRef.ref !== 0, 'makeAnimRef returns packed anim child ref');
        world.clearRegion(rid);
        // At targetDepth=2 (4x4x4 cells over [-4, +4]^3), place typed batch + carved cells.
        const typed = new Uint32Array(5 * 2);
        typed.set([0, 1, 0, merged.ref, merged.isLeaf ? 1 : 0, 3, 1, 3, animRef.ref, animRef.isLeaf ? 1 : 0]);
        assert(world.placeBatchTyped(rid, typed, 2, 2), 'placeBatchTyped accepts stride-5 Uint32Array');
        world.clearRegion(rid);
        for (let oz = 1; oz <= 2; oz++) {
            for (let ox = 1; ox <= 2; ox++) {
                world.stampCarveAndSolid(rid, ox, 1, oz, carveBox, warmFloor, 2);
                world.stampCarveAndSolid(rid, ox, 2, oz, carveBox, null, 2);
            }
        }
        const anchoredBufs = world.buffers([0, 0, 0]);
        assert(anchoredBufs.regionCount === 1, 'buffers([ax, ay, az]) synced active region');
        const bufs = world.buffers();
        assert(bufs.regionCount === 1, 'buffers() synced active region');

        const terrain = s.scene.createMesh(Object.assign(makePlane(10, 40), {
            color: [0.12, 0.78, 0.18],
            roughness: 0.85,
            castsShadow: false,
        }));
        terrain.setShader({
            module: `
                module hook;
                import scene;
                import mesh;
                import voxeldag;

                struct Params { region_count: f32, anim_tick: f32, pad0: f32, pad1: f32 }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 14) var regions: read_buffer<vec4<f32>>;
                @binding(4, 15) var nodes: read_buffer<vec4<u32>>;
                @binding(4, 16) var children: read_buffer<u32>;
                @binding(4, 17) var bricks: read_buffer<vec4<u32>>;
                @binding(4, 18) var anim_clips: read_buffer<vec4<u32>>;
                @binding(4, 19) var anim_frames: read_buffer<u32>;

                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    let wp = v.world_pos + scene::camera.eye_world.xyz;
                    if (voxeldag::is_carved_at(wp, params.region_count as u32, params.anim_tick as u32,
                                                 regions, nodes, children, bricks, anim_clips, anim_frames)) {
                        discard_fragment();
                    }
                    return s;
                }
            `,
        });
        terrain.setShaderUniform('region_count', bufs.regionCount);
        terrain.setShaderUniform('anim_tick', 0);
        terrain.setShaderBuffer('regions', bufs.regions);
        terrain.setShaderBuffer('nodes', bufs.nodes);
        terrain.setShaderBuffer('children', bufs.children);
        terrain.setShaderBuffer('bricks', bufs.bricks);
        terrain.setShaderBuffer('anim_clips', bufs.animClips);
        terrain.setShaderBuffer('anim_frames', bufs.animFrames);

        const img = s.scene.captureFrame();
        let greenPx = 0, orangePx = 0;
        for (let i = 0; i < img.data.length; i += 4) {
            const r = img.data[i], g = img.data[i + 1], b = img.data[i + 2];
            if (g > r + 25 && g > b + 25 && g > 45) greenPx++;
            if (r > g + 20 && r > b + 45 && r > 80) orangePx++;
        }
        assert(greenPx > 1000, `uncarved terrain rendered (${greenPx} green px)`);
        assert(orangePx > 200, `carved opening revealed orange voxel floor (${orangePx} orange px)`);

        document.body.removeChild(s.canvas);
        flush();
    }
}
