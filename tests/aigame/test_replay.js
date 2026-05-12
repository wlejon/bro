// Replay buffer + recorder/reader round trip.

const G = bro.ai.game;

// --- learn.createReplayBuffer ---
{
    const buf = G.learn.createReplayBuffer(8);
    assert(buf.capacity === 8, 'capacity=8, got ' + buf.capacity);
    assert(buf.size === 0, 'size=0 initial, got ' + buf.size);

    // Push a minimal Situation. We don't know the exact required shape so try.
    const sit = {
        obs: new Float32Array(G.OBS_TOTAL),
        atkMask: new Float32Array(8),
        abilMask: new Float32Array(4),
        targetMove: new Float32Array(G.nn.N_MOVE),
        targetAttack: new Float32Array(G.nn.N_ATTACK),
        targetAbility: new Float32Array(G.nn.N_ABILITY),
        valueTarget: 0.5,
    };
    let pushOk = true;
    try { buf.push(sit); } catch (e) { pushOk = false; console.log('push threw: ' + e); }
    assert(pushOk, 'replay push did not throw');
    assert(buf.size === 1, 'size=1 after push, got ' + buf.size);

    const batch = buf.sample(1);
    assert(Array.isArray(batch), 'sample returns array');
    assert(batch.length === 1, 'batch len=1, got ' + batch.length);

    buf.clear();
    assert(buf.size === 0, 'cleared');
}

// --- .bgar recorder round trip ---
{
    const path = 'test_replay_tmp.bgar';
    const w = G.createWorld();
    const a = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, x: 0, z: 0 });
    const b = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, x: 5, z: 0 });
    w.addAgent(a); w.addAgent(b);

    const rec = G.createRecorder();
    const opened = rec.open(path, 1, 12345, 1/60);
    if (!opened) {
        // some environments may not allow writing in CWD — flag but don't crash
        console.log('recorder.open returned false; skipping round-trip');
    } else {
        assert(rec.isOpen, 'rec.isOpen=true');
        rec.writeRoster(w);
        for (let i = 0; i < 5; i++) {
            w.tick(1/60);
            rec.recordFrame(i, i * (1/60), w);
        }
        assert(rec.frameCount === 5, 'frameCount=5, got ' + rec.frameCount);
        rec.close();

        const rr = G.createReplayReader();
        const okOpen = rr.open(path);
        assert(okOpen, 'reader.open returned true');
        assert(rr.frameCount === 5, 'reader.frameCount=5, got ' + rr.frameCount);

        const f0 = rr.frame(0);
        assert(f0 && Array.isArray(f0.agents), 'frame(0).agents is array');
        assert(f0.agents.length === 2, 'frame has 2 agents, got ' + f0.agents.length);

        const traj = rr.trajectory(1);
        assert(Array.isArray(traj), 'trajectory is array');
        assert(traj.length === 5, 'trajectory has 5 entries, got ' + traj.length);

        const dmg = rr.damageSummary();
        assert(Array.isArray(dmg), 'damageSummary is array');
    }
}

console.log('test_replay: OK');
