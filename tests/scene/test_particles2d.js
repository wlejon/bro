// scene.createParticles (the 2D ParticleNode): the emitter options reach the
// node, and liveCount / rate / isPlaying read it. The bronze wrapper forwarded
// only maxParticles/texture/position/visible, and the getters answered only
// Particles3D, so a 2D node always read 0 live particles.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '320');
canvas.setAttribute('height', '240');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping 2D particles test');
} else {
    // Burst at creation, then every particle expires after its lifetime.
    const p = scene.createParticles({
        rate: 0, burst: 30, maxParticles: 64,
        lifetime: { min: 0.2, max: 0.2 },
        velocity: { angle: 0, angleSpread: 360, speed: 50, speedSpread: 0 },
        size: { start: 4, end: 0 },
        color: { start: '#ffffff', end: '#ffffff00' },
        blend: 'additive',
    });
    assert(p.type === 'particles', 'a 2D particle node: ' + p.type);
    assert(p.liveCount === 30, 'burst option populates 30, got ' + p.liveCount);
    assert(p.particleCount === 30, 'particleCount aliases liveCount');
    assert(p.isPlaying === true, 'autoplay by default');
    advanceTime(300);
    assert(p.liveCount === 0, 'all expired after the 0.2 s lifetime, got ' + p.liveCount);
    p.destroy();

    // maxParticles caps a burst; clear() empties the pool.
    const cap = scene.createParticles({ rate: 0, maxParticles: 5, lifetime: 1 });
    cap.burst(20);
    assert(cap.liveCount === 5, 'capped at 5, got ' + cap.liveCount);
    cap.clear();
    assert(cap.liveCount === 0, 'cleared');
    cap.destroy();

    // rate emits over time; the rate getter/setter reach the node.
    const r = scene.createParticles({ rate: 100, maxParticles: 256, lifetime: { min: 5, max: 5 },
                                      velocity: { speed: 0, speedSpread: 0 } });
    assert(r.rate === 100, 'rate option reads back, got ' + r.rate);
    advanceTime(1000);
    assert(r.liveCount >= 90 && r.liveCount <= 110, 'about 100 emitted in 1 s at rate 100, got ' + r.liveCount);
    r.rate = 0;
    assert(r.rate === 0, 'rate setter reaches the node');
    const before = r.liveCount;
    advanceTime(500);
    assert(r.liveCount === before, 'rate 0 emits nothing more');
    r.stop();
    assert(r.isPlaying === false, 'stop() stops');
    r.destroy();

    // autoplay: false holds the emitter until play().
    const held = scene.createParticles({ rate: 100, autoplay: false, lifetime: 5, x: 40, y: 50, name: 'held' });
    assert(held.isPlaying === false, 'autoplay false starts stopped');
    assert(held.name === 'held' && held.x === 40 && held.y === 50, 'name / x / y reach the node');
    advanceTime(200);
    assert(held.liveCount === 0, 'nothing emitted while stopped');
    held.play();
    advanceTime(200);
    assert(held.liveCount > 0, 'play() starts emitting');
    held.destroy();

    console.log('particles2d: ok');
}
