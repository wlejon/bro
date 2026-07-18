// Test scene.createAnimationPlayer — data-driven multi-track keyframe clips
// for arbitrary scene-node properties (src/scene/clip_player.cpp + the
// clipDef bindings in src/js/scene_bindings_clip.cpp). Covers: linear / step /
// cubic (Catmull-Rom) interpolation against analytic values (via seek(), which
// is exact), per-key easing, quaternion slerp shortest path, scalar props
// (light intensity, camera fov), event tracks (exactly once, per-loop re-fire,
// args round-trip, no retro-fire on seek), loop and pingpong modes, reverse
// playback, live speed flips, crossfade blending (matched, unmatched, and
// outgoing-only tracks), clipDef JSON round-trip, last-writer-wins vs tweens,
// bro.time.scale, destroyed-node and destroyed-player safety, and clear
// errors for malformed defs. All timing via advanceTime() virtual time.

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
    console.log('scene context not available (no GPU) — skipping clip test');
} else {
    // ------------------------------------------------------------------
    // Linear interpolation: exact values via seek(), playback via time
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('lin');
        const p = scene.createAnimationPlayer();
        p.addClip('move', {
            duration: 1,
            tracks: [{ target: 'lin', property: 'position', keys: [
                { time: 0, value: [0, 0, 0] },
                { time: 1, value: [10, -4, 2] },
            ]}],
        });
        p.play('move');
        assert(p.playing === true, 'playing after play');
        assert(p.currentClip === 'move', 'currentClip readback');
        p.seek(0.5);
        near(n.position[0], 5, 1e-5, 'linear x at t=0.5 (seek exact)');
        near(n.position[1], -2, 1e-5, 'linear y at t=0.5');
        near(n.position[2], 1, 1e-5, 'linear z at t=0.5');
        near(p.currentTime, 0.5, 1e-6, 'currentTime after seek');
        advanceTime(300);
        near(n.position[0], 8, 0.3, 'playback continues from seek');
        advanceTime(400);
        near(n.position[0], 10, 1e-5, 'lands exactly on the final key');
        assert(p.playing === false, 'non-looping clip stops at the end');
        near(p.currentTime, 1, 1e-6, 'holds duration when finished');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Step + cubic (Catmull-Rom) exact values, per-key easing
    // ------------------------------------------------------------------
    {
        const light = scene.createLight({ name: 'lampA', type: 'point',
                                          color: [1, 1, 1], intensity: 0 });
        const p = scene.createAnimationPlayer();
        // Catmull-Rom over keys t=[0,1,2,3], v=[0,1,0,1]:
        //   tangents m0=(1-0)/1=1, m1=(0-0)/2=0, m2=(1-1)/2=0, m3=(1-0)/1=1
        //   t=0.5 (seg 0): h10*1 + h01*1 = 0.125 + 0.5 = 0.625
        //   t=1.5 (seg 1): h00*1 = 0.5
        p.addClip('cubic', {
            duration: 3,
            tracks: [{ target: 'lampA', property: 'intensity', keys: [
                { time: 0, value: 0, interp: 'cubic' },
                { time: 1, value: 1, interp: 'cubic' },
                { time: 2, value: 0, interp: 'cubic' },
                { time: 3, value: 1, interp: 'cubic' },
            ]}],
        });
        p.play('cubic');
        p.seek(0.5);
        near(light.intensity, 0.625, 1e-5, 'Catmull-Rom seg 0 midpoint');
        p.seek(1.5);
        near(light.intensity, 0.5, 1e-5, 'Catmull-Rom seg 1 midpoint');
        p.seek(1.0);
        near(light.intensity, 1, 1e-5, 'cubic passes through keys');

        p.addClip('step', {
            duration: 2,
            tracks: [{ target: 'lampA', property: 'intensity', keys: [
                { time: 0, value: 3, interp: 'step' },
                { time: 1, value: 7, interp: 'step' },
            ]}],
        });
        p.play('step');
        p.seek(0.999);
        near(light.intensity, 3, 1e-5, 'step holds until the next key');
        p.seek(1.0);
        near(light.intensity, 7, 1e-5, 'step jumps exactly at the key');

        // quadIn ease on a linear segment: v = 8 * u^2 at u=0.5 -> 2
        p.addClip('eased', {
            duration: 1,
            tracks: [{ target: 'lampA', property: 'intensity', keys: [
                { time: 0, value: 0, ease: 'quadIn' },
                { time: 1, value: 8 },
            ]}],
        });
        p.play('eased');
        p.seek(0.5);
        near(light.intensity, 2, 1e-5, 'per-key quadIn ease warps the segment');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Rotation: slerp midpoint + shortest path across the far hemisphere
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('rot');
        const p = scene.createAnimationPlayer();
        p.addClip('turn', {
            duration: 1,
            tracks: [{ target: 'rot', property: 'rotation', keys: [
                { time: 0, value: { euler: [0, 0, 0] } },
                { time: 1, value: { axis: [0, 0, 1], angle: Math.PI / 2 } },
            ]}],
        });
        p.play('turn');
        p.seek(0.5);
        const q = n.quaternion;
        near(q[2], Math.sin(Math.PI / 8), 1e-5, 'slerp midpoint qz (45 deg)');
        near(q[3], Math.cos(Math.PI / 8), 1e-5, 'slerp midpoint qw');

        // 170 deg -> -170 deg about Z: shortest path goes through 180 deg
        // (|qz| ~ 1), NOT back through 0 (which would give qz ~ 0).
        const a170 = 170 * Math.PI / 180;
        p.addClip('short', {
            duration: 1,
            tracks: [{ target: 'rot', property: 'rotation', keys: [
                { time: 0, value: { axis: [0, 0, 1], angle: a170 } },
                { time: 1, value: { axis: [0, 0, 1], angle: -a170 } },
            ]}],
        });
        p.play('short');
        p.seek(0.5);
        const qs = n.quaternion;
        near(Math.abs(qs[2]), 1, 1e-4, 'slerp takes the shortest path (180 deg)');
        near(qs[3], 0, 1e-4, 'shortest-path midpoint qw ~ 0');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Multi-track, multi-node clip + camera fov (degrees, like camera.fov)
    // ------------------------------------------------------------------
    {
        const a = scene.createNode('multiA');
        const b = scene.createNode('multiB');
        const cam = scene.createCamera({ name: 'cam', fov: 60 });
        const p = scene.createAnimationPlayer();
        p.addClip('combo', {
            duration: 1,
            tracks: [
                { target: 'multiA', property: 'position', keys: [
                    { time: 0, value: [0, 0, 0] }, { time: 1, value: [4, 0, 0] }]},
                { target: 'multiA', property: 'scale', keys: [
                    { time: 0, value: 1 }, { time: 1, value: 3 }]},
                { target: 'multiB', property: 'position', keys: [
                    { time: 0, value: [0, 0, 0] }, { time: 1, value: [0, 0, -6] }]},
                { target: 'cam', property: 'fov', keys: [
                    { time: 0, value: 60 }, { time: 1, value: 30 }]},
            ],
        });
        p.play('combo');
        p.seek(0.5);
        near(a.position[0], 2, 1e-5, 'track 1: node A position');
        near(a.scaleX, 2, 1e-5, 'track 2: node A scale (uniform splat)');
        near(b.position[2], -3, 1e-5, 'track 3: node B position');
        near(cam.fov, 45, 1e-3, 'track 4: camera fov in degrees');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Event track: exactly once per pass, args round-trip, key at t=0
    // fires on play, callbacks see (name, args)
    // ------------------------------------------------------------------
    {
        const p = scene.createAnimationPlayer();
        const fired = [];
        p.onEvent = (name, args) => fired.push([name, args]);
        p.addClip('cues', {
            duration: 1,
            tracks: [
                { type: 'event', keys: [
                    { time: 0, name: 'start' },
                    { time: 0.5, name: 'mid', args: { pitch: 2, tags: ['a', 'b'] } },
                    { time: 1, name: 'end' },
                ]},
            ],
        });
        p.play('cues');
        advanceTime(2000);
        assert(fired.length === 3, `all events fired exactly once (${fired.length})`);
        assert(fired[0][0] === 'start', 'event at t=0 fires on play');
        assert(fired[1][0] === 'mid' && fired[2][0] === 'end', 'events in order');
        assert(fired[0][1] === undefined, 'no args -> undefined');
        assert(fired[1][1].pitch === 2 && fired[1][1].tags[1] === 'b',
               'event args round-trip by value');
        advanceTime(500);
        assert(fired.length === 3, 'finished clip fires nothing more');

        // Looping: each key re-fires every pass.
        const counts = {};
        p.onEvent = (name) => { counts[name] = (counts[name] || 0) + 1; };
        p.addClip('loopCues', {
            duration: 0.5,
            loop: 'loop',
            tracks: [{ type: 'event', keys: [
                { time: 0, name: 'tick' },
                { time: 0.25, name: 'tock' },
            ]}],
        });
        p.play('loopCues');
        advanceTime(1400);   // just under 3 passes (margin vs tick granularity)
        p.stop();
        assert(counts.tick === 3, `loop re-fires t=0 key each pass (${counts.tick})`);
        assert(counts.tock === 3, `loop re-fires mid key each pass (${counts.tock})`);
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Seek does not retro-fire skipped events (Godot semantics)
    // ------------------------------------------------------------------
    {
        const p = scene.createAnimationPlayer();
        const fired = [];
        p.onEvent = (name) => fired.push(name);
        p.addClip('skip', {
            duration: 1,
            tracks: [{ type: 'event', keys: [
                { time: 0, name: 'a' },
                { time: 0.5, name: 'b' },
                { time: 0.9, name: 'c' },
            ]}],
        });
        p.play('skip');
        p.seek(0.6);         // skips a and b; the key at 0.6 wouldn't fire either
        advanceTime(1000);
        assert(fired.length === 1 && fired[0] === 'c',
               `seek past events does not retro-fire (${JSON.stringify(fired)})`);
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Reverse playback: values mirror, events fire in reverse, finishes at 0
    // ------------------------------------------------------------------
    {
        const light = scene.createLight({ name: 'lampR', type: 'point',
                                          color: [1, 1, 1], intensity: 0 });
        const p = scene.createAnimationPlayer();
        const fired = [];
        let finishes = 0;
        p.onEvent = (name) => fired.push(name);
        p.onFinished = () => finishes++;
        p.addClip('ramp', {
            duration: 1,
            tracks: [
                { target: 'lampR', property: 'intensity', keys: [
                    { time: 0, value: 0 }, { time: 1, value: 10 }]},
                { type: 'event', keys: [
                    { time: 0.25, name: 'lo' }, { time: 0.75, name: 'hi' }]},
            ],
        });
        p.play('ramp', { speed: -1 });   // from defaults to duration in reverse
        near(p.currentTime, 1, 1e-6, 'reverse starts at duration');
        advanceTime(500);
        near(light.intensity, 5, 0.3, 'reverse midpoint value');
        advanceTime(600);
        near(light.intensity, 0, 1e-5, 'reverse lands exactly on key 0');
        assert(finishes === 1, `onFinished fired once in reverse (${finishes})`);
        assert(fired.length === 2 && fired[0] === 'hi' && fired[1] === 'lo',
               `reverse fires events in reverse order (${JSON.stringify(fired)})`);

        // Live speed flip mid-play reverses direction.
        p.play('ramp');
        advanceTime(400);
        const atFlip = light.intensity;
        p.speed = -1;
        advanceTime(200);
        assert(light.intensity < atFlip, 'speed = -1 mid-play reverses');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Loop wraps, pingpong reflects
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('looper');
        const p = scene.createAnimationPlayer();
        const def = {
            duration: 1,
            loop: 'loop',
            tracks: [{ target: 'looper', property: 'position', keys: [
                { time: 0, value: [0, 0, 0] }, { time: 1, value: [10, 0, 0] }]}],
        };
        p.addClip('wrap', def);
        p.play('wrap');
        advanceTime(1250);   // second pass, t=0.25
        near(n.position[0], 2.5, 0.3, 'loop wraps to the start');
        assert(p.playing === true, 'looping clip keeps playing');
        p.stop();
        assert(p.playing === false && p.currentClip === '', 'stop clears state');

        p.addClip('pp', { ...def, loop: 'pingpong' });
        p.play('pp');
        advanceTime(1250);   // reflected pass, t=0.75 descending
        near(n.position[0], 7.5, 0.3, 'pingpong reflects at the end');
        advanceTime(1000);   // 2.25 total -> forward again, t=0.25
        near(n.position[0], 2.5, 0.3, 'pingpong turns forward again');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Crossfade: matched tracks blend, unmatched blend from the captured
    // value, outgoing-only tracks keep writing during the fade
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('fader');
        const light = scene.createLight({ name: 'lampF', type: 'point',
                                          color: [1, 1, 1], intensity: 4 });
        const p = scene.createAnimationPlayer();
        p.addClip('poseA', {
            duration: 1, loop: 'loop',
            tracks: [
                { target: 'fader', property: 'position', keys: [{ time: 0, value: [0, 0, 0] }]},
                { target: 'lampF', property: 'intensity', keys: [{ time: 0, value: 8 }]},
            ],
        });
        p.addClip('poseB', {
            duration: 1, loop: 'loop',
            tracks: [
                { target: 'fader', property: 'position', keys: [{ time: 0, value: [10, 0, 0] }]},
                { target: 'fader', property: 'scale', keys: [{ time: 0, value: 3 }]},
            ],
        });
        p.play('poseA');
        advanceTime(100);
        near(light.intensity, 8, 1e-5, 'clip A drives the light');
        p.play('poseB', { fade: 1 });
        assert(p.currentClip === 'poseB', 'currentClip switches at play');
        advanceTime(500);
        near(n.position[0], 5, 0.3, 'matched track blends A->B at midpoint');
        near(n.scaleX, 2, 0.3, 'unmatched track blends from captured value');
        near(light.intensity, 8, 1e-5, 'outgoing-only track keeps writing');
        advanceTime(600);
        near(n.position[0], 10, 1e-5, 'fade completes on B');
        near(n.scaleX, 3, 1e-5, 'unmatched track lands on B');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Crossfade mutes the outgoing clip's event tracks
    // ------------------------------------------------------------------
    {
        const p = scene.createAnimationPlayer();
        const fired = [];
        p.onEvent = (name) => fired.push(name);
        p.addClip('noisy', {
            duration: 0.5, loop: 'loop',
            tracks: [{ type: 'event', keys: [{ time: 0.25, name: 'old' }]}],
        });
        p.addClip('quiet', {
            duration: 2, loop: 'loop',
            tracks: [{ type: 'event', keys: [{ time: 1.5, name: 'new' }]}],
        });
        p.play('noisy');
        advanceTime(400);    // 'old' fired once
        p.play('quiet', { fade: 1 });
        advanceTime(1600);   // outgoing would cross 0.25 repeatedly if unmuted
        assert(!fired.slice(1).includes('old'),
               `outgoing events stop at the switch (${JSON.stringify(fired)})`);
        assert(fired.includes('new'), 'incoming events fire during the fade');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // clipDef round-trips as plain JSON
    // ------------------------------------------------------------------
    {
        const p = scene.createAnimationPlayer();
        const def = {
            duration: 2,
            loop: 'pingpong',
            tracks: [
                { target: 'door', property: 'rotation', keys: [
                    { time: 0, value: { euler: [0, 0, 0] }, interp: 'cubic', ease: 'quadOut' },
                    { time: 2, value: { euler: [0, 1.2, 0] } },
                ]},
                { type: 'event', keys: [{ time: 1, name: 'creak', args: { vol: 0.5 } }]},
            ],
        };
        p.addClip('door', def);
        assert(JSON.stringify(p.clipDef('door')) === JSON.stringify(def),
               'clipDef round-trips the exact JSON');
        assert(p.clipDef('nope') === null, 'unknown clip name reads null');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Coexistence: clip players tick after tweens -> last writer wins
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('contested');
        scene.createTween().to(n, { position: [100, 0, 0] }, 1).start();
        const p = scene.createAnimationPlayer();
        p.addClip('hold', {
            duration: 1, loop: 'loop',
            tracks: [{ target: 'contested', property: 'position',
                       keys: [{ time: 0, value: [5, 0, 0] }]}],
        });
        p.play('hold');
        advanceTime(500);
        near(n.position[0], 5, 1e-5, 'clip player wins over the tween (ticks later)');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // bro.time.scale: clips run on the scaled clock
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('scaled');
        const p = scene.createAnimationPlayer();
        p.addClip('slow', {
            duration: 1,
            tracks: [{ target: 'scaled', property: 'position', keys: [
                { time: 0, value: [0, 0, 0] }, { time: 1, value: [10, 0, 0] }]}],
        });
        bro.time.scale = 2;
        p.play('slow');
        advanceTime(250);    // 250ms real -> 500ms scaled
        bro.time.scale = 1;
        near(p.currentTime, 0.5, 0.05, 'bro.time.scale doubles clip time');
        near(n.position[0], 5, 0.5, 'value follows the scaled clock');
        p.destroy();
    }

    // ------------------------------------------------------------------
    // Safety: destroyed node mid-play, pause/resume, player destroy
    // ------------------------------------------------------------------
    {
        const n = scene.createNode('doomed');
        const p = scene.createAnimationPlayer();
        p.addClip('run', {
            duration: 1, loop: 'loop',
            tracks: [{ target: 'doomed', property: 'position', keys: [
                { time: 0, value: [0, 0, 0] }, { time: 1, value: [10, 0, 0] }]}],
        });
        p.play('run');
        advanceTime(200);
        n.destroy();
        advanceTime(600);    // must not crash; player keeps ticking
        assert(p.playing === true, 'player survives a destroyed target');

        p.pause();
        const t0 = p.currentTime;
        advanceTime(300);
        near(p.currentTime, t0, 1e-6, 'paused player consumes no time');
        p.resume();
        advanceTime(100);
        assert(p.currentTime !== t0, 'resume continues');

        p.destroy();
        let threw = false;
        try { p.play('run'); } catch (e) { threw = true; }
        assert(threw, 'play on a destroyed player throws');
        assert(p.playing === false, 'destroyed player reads not playing');
    }

    // ------------------------------------------------------------------
    // Clear errors: unknown property / interp / ease / loop, missing node,
    // property/node-type mismatch, unknown clip
    // ------------------------------------------------------------------
    {
        const p = scene.createAnimationPlayer();
        const throws = (fn, label) => {
            let threw = false;
            let msg = '';
            try { fn(); } catch (e) { threw = true; msg = String(e.message || e); }
            assert(threw, label);
            return msg;
        };

        const msg = throws(() => p.addClip('bad', {
            tracks: [{ target: 'x', property: 'wobble',
                       keys: [{ time: 0, value: 1 }]}],
        }), 'unknown property throws');
        assert(msg.includes('wobble') && msg.includes('position'),
               `unknown-property error lists the supported set (${msg})`);

        throws(() => p.addClip('bad', {
            tracks: [{ target: 'x', property: 'opacity',
                       keys: [{ time: 0, value: 1, interp: 'bezier' }]}],
        }), 'unknown interp throws');
        throws(() => p.addClip('bad', {
            tracks: [{ target: 'x', property: 'opacity',
                       keys: [{ time: 0, value: 1, ease: 'bogus' }]}],
        }), 'unknown ease throws');
        throws(() => p.addClip('bad', {
            loop: 'bounce',
            tracks: [{ target: 'x', property: 'opacity',
                       keys: [{ time: 0, value: 1 }]}],
        }), 'unknown loop mode throws');

        throws(() => p.play('never'), 'unknown clip name throws');

        p.addClip('ghost', {
            tracks: [{ target: 'no-such-node', property: 'position',
                       keys: [{ time: 0, value: [0, 0, 0] }]}],
        });
        const msg2 = throws(() => p.play('ghost'), 'unresolvable target throws');
        assert(msg2.includes('no-such-node'), `error names the target (${msg2})`);

        scene.createNode('plainNode');
        p.addClip('mismatch', {
            tracks: [{ target: 'plainNode', property: 'fov',
                       keys: [{ time: 0, value: 60 }]}],
        });
        throws(() => p.play('mismatch'), 'property/node-type mismatch throws');
        p.destroy();
    }

    console.log('animation clip tests passed');
}
