// Test scene.createTween — the engine-ticked property tween system.
// Exercises src/scene/tween.cpp and the Tween bindings in
// src/js/scene_bindings.cpp: linear + eased interpolation against analytic
// formulas, chained steps (including tick-overshoot carry across step
// boundaries), parallel properties and parallel() cross-node merging, delay,
// wait steps, loop counts, callback steps, onUpdate custom tweens, rotation
// slerp, light color tweening, stop/pause/resume, onFinished, and destroy.
// All timing is driven through advanceTime() virtual time (16 ms steps), so
// every assertion is deterministic.

function near(a, b, tol, label) {
    assert(Math.abs(a - b) < tol, `${label}: ${a} vs ${b}`);
}

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping tween test');
} else {
    // ------------------------------------------------------------------
    // Linear position tween: exactly half-way at half-duration
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('lin');
        const tw = scene.createTween().to(n, { position: [10, -4, 2] }, 1);
        assert(tw.isRunning === false, 'not running before start');
        tw.start();
        assert(tw.isRunning === true, 'running after start');
        advanceTime(500);
        near(n.position[0], 5, 1e-2, 'linear x at t=0.5');
        near(n.position[1], -2, 1e-2, 'linear y at t=0.5');
        near(n.position[2], 1, 1e-2, 'linear z at t=0.5');
        advanceTime(600);
        near(n.position[0], 10, 1e-5, 'linear x lands exactly');
        near(n.position[1], -4, 1e-5, 'linear y lands exactly');
        assert(tw.isRunning === false, 'stopped after completing');
        assert(tw.isFinished === true, 'isFinished set');
    }

    // ------------------------------------------------------------------
    // Easing correctness: quadIn against the analytic curve
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('ease');
        scene.createTween()
            .to(n, { position: [8, 0, 0] }, 1, { easing: 'quadIn' })
            .start();
        advanceTime(250);                    // t=0.25 -> e=0.0625 -> x=0.5
        near(n.position[0], 8 * 0.25 * 0.25, 2e-2, 'quadIn at t=0.25');
        advanceTime(500);                    // t=0.75 -> e=0.5625 -> x=4.5
        near(n.position[0], 8 * 0.75 * 0.75, 2e-2, 'quadIn at t=0.75');
        advanceTime(300);
        near(n.position[0], 8, 1e-5, 'quadIn lands exactly');

        let threw = false;
        try {
            scene.createTween().to(n, { position: [0, 0, 0] }, 1, { easing: 'bogus' });
        } catch (e) { threw = true; }
        assert(threw, 'unknown easing name throws');
    }

    // ------------------------------------------------------------------
    // Chained steps run sequentially; overshoot carries across boundaries
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('chain');
        scene.createTween()
            .to(n, { position: [1, 0, 0] }, 0.5)
            .to(n, { position: [1, 2, 0] }, 0.5)
            .start();
        advanceTime(250);
        near(n.position[0], 0.5, 1e-2, 'step 1 midway');
        near(n.position[1], 0, 1e-6, 'step 2 not started yet');
        // 250 -> 600 ms crosses the step boundary inside one span of ticks;
        // the 100 ms overshoot must land inside step 2 (t = 0.2 -> y = 0.4).
        advanceTime(350);
        near(n.position[0], 1, 1e-5, 'step 1 completed exactly');
        near(n.position[1], 0.4, 2e-2, 'overshoot carried into step 2');
        advanceTime(450);
        near(n.position[1], 2, 1e-5, 'step 2 lands exactly');
    }

    // ------------------------------------------------------------------
    // Parallel properties in one to(); parallel() merges across nodes
    // ------------------------------------------------------------------
    {
        const a = scene.createNode('parA');
        const b = scene.createNode('parB');
        scene.createTween()
            .to(a, { position: [4, 0, 0], scale: [3, 3, 3] }, 1)
            .parallel()
            .to(b, { position: [0, 0, -6] }, 1)
            .start();
        advanceTime(500);
        near(a.position[0], 2, 1e-2, 'parallel prop: position together');
        near(a.scaleX, 2, 1e-2, 'parallel prop: scale together (from 1 to 3)');
        near(b.position[2], -3, 1e-2, 'parallel() merged node B into the step');
        advanceTime(600);
        near(a.scaleX, 3, 1e-5, 'scale lands exactly');
        near(b.position[2], -6, 1e-5, 'node B lands exactly');
    }

    // ------------------------------------------------------------------
    // Rotation: quaternion slerp via {axis, angle}
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('rot');
        scene.createTween()
            .to(n, { rotation: { axis: [0, 0, 1], angle: Math.PI / 2 } }, 1)
            .start();
        advanceTime(500);                    // slerp midpoint = 45 deg about Z
        const q = n.quaternion;
        near(q[2], Math.sin(Math.PI / 8), 1e-2, 'slerp midpoint qz');
        near(q[3], Math.cos(Math.PI / 8), 1e-2, 'slerp midpoint qw');
        advanceTime(600);
        const qe = n.quaternion;
        near(qe[2], Math.SQRT1_2, 1e-4, 'rotation lands on 90 deg (qz)');
        near(qe[3], Math.SQRT1_2, 1e-4, 'rotation lands on 90 deg (qw)');
    }

    // ------------------------------------------------------------------
    // Color tween on a light (readable via light.color)
    // ------------------------------------------------------------------
    {
        const light = scene.createLight({ type: 'point', color: [1, 1, 1],
                                          intensity: 1 });
        scene.createTween().to(light, { color: [1, 0, 0] }, 1).start();
        advanceTime(500);
        near(light.color[1], 0.5, 1e-2, 'light green channel at half');
        advanceTime(600);
        near(light.color[1], 0, 1e-5, 'light color lands exactly');
        near(light.color[0], 1, 1e-5, 'red channel untouched');
    }

    // ------------------------------------------------------------------
    // Delay + pure wait steps
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('delay');
        scene.createTween()
            .to(n, { position: [2, 0, 0] }, 0.5, { delay: 0.5 })
            .start();
        advanceTime(400);
        near(n.position[0], 0, 1e-6, 'nothing moves during the delay');
        advanceTime(350);                    // 0.75 total -> t = 0.5
        near(n.position[0], 1, 2e-2, 'delayed anim midway');

        const m = scene.createNode('wait');
        scene.createTween()
            .to(null, {}, 0.3)               // pure wait step
            .to(m, { position: [1, 0, 0] }, 0.2)
            .start();
        advanceTime(250);
        near(m.position[0], 0, 1e-6, 'wait step holds the sequence');
        advanceTime(150);                    // 0.4 total -> step 2 t = 0.5
        near(m.position[0], 0.5, 2e-2, 'anim after the wait step');
    }

    // ------------------------------------------------------------------
    // call() steps + loop(n) honored + onFinished fires once
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('loop');
        let calls = 0, finishes = 0;
        const tw = scene.createTween()
            .to(n, { position: [1, 0, 0] }, 0.25)
            .call(() => calls++)
            .loop(3);
        tw.onFinished = () => finishes++;
        tw.start();
        advanceTime(2000);                   // 3 x 0.25 s, plenty of slack
        assert(calls === 3, `call fired once per loop iteration (${calls})`);
        assert(finishes === 1, `onFinished fired exactly once (${finishes})`);
        assert(tw.isFinished === true, 'loop(3) sequence finished');
        advanceTime(500);
        assert(finishes === 1, 'onFinished does not re-fire');
    }

    // ------------------------------------------------------------------
    // onUpdate custom tween: eased t delivered every tick, ends on 1
    // ------------------------------------------------------------------
    {
        const samples = [];
        scene.createTween()
            .to(null, {}, 0.5, { easing: 'quadInOut', onUpdate: (t) => samples.push(t) })
            .start();
        advanceTime(250);
        assert(samples.length > 10, `onUpdate ran every tick (${samples.length})`);
        near(samples[samples.length - 1], 0.5, 2e-2, 'quadInOut(0.5) = 0.5');
        for (let i = 1; i < samples.length; i++)
            assert(samples[i] >= samples[i - 1], 'onUpdate t monotonic');
        advanceTime(300);
        near(samples[samples.length - 1], 1, 1e-6, 'final onUpdate is exactly 1');
    }

    // ------------------------------------------------------------------
    // Infinite loop + stop() halts; pause/resume
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('inf');
        let ticks = 0;
        const tw = scene.createTween()
            .to(n, { position: [1, 0, 0] }, 0.1)
            .call(() => ticks++)
            .loop()                          // forever
            .start();
        advanceTime(1000);
        assert(ticks >= 8, `infinite loop keeps cycling (${ticks})`);
        assert(tw.isRunning === true, 'still running');
        tw.stop();
        const frozen = ticks;
        advanceTime(500);
        assert(ticks === frozen, 'stop() halts the tween');
        assert(tw.isRunning === false, 'stopped');
        assert(tw.isFinished === false, 'stop() does not mark finished');

        tw.start();                          // restart from the top
        tw.pause();
        advanceTime(300);
        assert(ticks === frozen, 'paused tween consumes no time');
        tw.resume();
        advanceTime(300);
        assert(ticks > frozen, 'resume() continues');
        tw.stop();
    }

    // ------------------------------------------------------------------
    // destroy(): tween is gone, later use throws
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('dead');
        const tw = scene.createTween().to(n, { position: [1, 0, 0] }, 1).start();
        advanceTime(100);
        tw.destroy();
        const x = n.position[0];
        advanceTime(300);
        near(n.position[0], x, 1e-6, 'destroyed tween stops writing');
        let threw = false;
        try { tw.start(); } catch (e) { threw = true; }
        assert(threw, 'start() on a destroyed tween throws');
        assert(tw.isRunning === false, 'destroyed tween reads as not running');
    }

    // ------------------------------------------------------------------
    // Tweening a destroyed node is harmless
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('gone');
        const tw = scene.createTween().to(n, { position: [5, 0, 0] }, 0.5).start();
        advanceTime(100);
        n.destroy();
        advanceTime(600);                    // must not crash
        assert(tw.isFinished === true, 'tween finishes past a destroyed node');
    }

    console.log('tween tests passed');
}
