// First defineScene asset: a rotating colored cube on a transparent
// background, lit by a sun-like directional light + soft ambient. Doubles
// as the scene-capture transparency smoke test — pixels outside the cube
// silhouette must read RGBA(0,0,0,0) end-to-end (mesh FBO clear → tonemap
// pass → readback → putImageData onto the sheet's transparent canvas).
//
// Build: bro-headless apps/artstation -e "load('td_test_scene'); render(); save();"
// Output: apps/artstation/output/td_test_scene.png + .json

defineScene('td_test_scene', {
    frameWidth: 64, frameHeight: 64,
    fps: 24, duration: 1.0,           // → 24 frames, full revolution
    cols: 8,                           // 8×3 sheet
    bg: 'transparent',
    pixel: false,                      // 3D output — let smoothing through

    build(scene) {
        // The hero. A small cube the camera will orbit visually as the
        // cube spins (cube spins in frame(); camera is fixed).
        const cube = scene.createMesh({
            mesh: 'box',
            halfW: 0.5, halfH: 0.5, halfD: 0.5,
            color: '#ff8030',
            metallic: 0.1,
            roughness: 0.45,
        });

        // Sun: directional, shining roughly down-and-forward.
        const sun = scene.createLight({
            type: 'directional',
            direction: [-0.5, -1.0, -0.3],
            color: [1.0, 0.97, 0.92],
            intensity: 2.5,
        });

        // A bit of soft fill so the dark sides of the cube don't crush.
        scene.setAmbient([0.08, 0.08, 0.10]);

        // Camera: 3/4 angle, looking at the cube. Quaternion form would
        // also work; setCamera with target+up reads cleaner for static
        // showcases like this.
        scene.setCamera({
            fov: 45,
            position: [2.2, 1.8, 2.6],
            target:   [0, 0, 0],
            up:       [0, 1, 0],
        });

        return { cube, sun };
    },

    frame(scene, t, dt, refs, i) {
        // Spin the cube one full revolution over the asset's duration (1s
        // at 24fps → 24 frames, frame 24 would land back on frame 0). The
        // produced sheet loops seamlessly when fed into createSprite.
        const phase = (t / 1.0) * Math.PI * 2;
        refs.cube.rotationY = phase;
        // Subtle vertical bob to give the test something more than yaw.
        refs.cube.y = Math.sin(phase) * 0.08;
    },

    animations: {
        spin: { frames: [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23],
                fps: 24, loop: true },
    },
});
