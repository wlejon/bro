// Particle-explosion animation, rendered procedurally over virtual time.
//
// Demonstrates the defineAnimated capture loop: state is seeded once in
// init(), then advanced + drawn every frame. The framework lays each
// captured frame into a regular spritesheet, so game code can play this
// back via scene.createSprite without knowing the source was procedural.

defineAnimated('explosion', {
    frameWidth: 32, frameHeight: 32,
    fps: 24, duration: 0.75,        // → 18 frames
    cols: 6,                         // 6×3 sheet
    bg: 'transparent',
    pixel: true,

    init() {
        // Deterministic PRNG so renders are byte-stable across runs.
        let seed = 0x9e3779b1;
        const rand = () => {
            seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
            return ((seed >>> 0) / 4294967296);
        };

        // Hot palette: yellow → orange → red → smoke.
        const PALETTE = ['#fff7c2', '#ffd34a', '#ff9020', '#e84418', '#7a2a1a', '#3a3236'];

        // Seed N particles from the center with random outward velocities.
        // Mix two cohorts: fast bright sparks and slower smoke remnants.
        const particles = [];
        const cx = 16, cy = 16;
        for (let i = 0; i < 28; i++) {
            const angle = rand() * Math.PI * 2;
            const speed = 28 + rand() * 36;        // px/sec
            particles.push({
                x: cx, y: cy,
                vx: Math.cos(angle) * speed,
                vy: Math.sin(angle) * speed,
                life: 0.45 + rand() * 0.25,        // seconds
                age: 0,
                hot: true,
            });
        }
        for (let i = 0; i < 14; i++) {
            const angle = rand() * Math.PI * 2;
            const speed = 8 + rand() * 14;
            particles.push({
                x: cx, y: cy,
                vx: Math.cos(angle) * speed,
                vy: Math.sin(angle) * speed - 6,   // smoke drifts up
                life: 0.5 + rand() * 0.25,
                age: 0,
                hot: false,
            });
        }
        return { particles, PALETTE };
    },

    frame(ctx, w, h, t, dt, state) {
        // Soft initial flash for first ~3 frames — a fading white disc at
        // the origin. Painted before particles so sparks read on top.
        if (t < 0.12) {
            const k = 1 - (t / 0.12);
            const r = 4 + k * 7;
            ctx.fillStyle = `rgba(255,247,210,${0.85 * k})`;
            brush.circle(ctx, 16, 16, r);
        }

        for (const p of state.particles) {
            // Step physics. Slight drag + gravity for arc shape.
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.vx *= 0.92;
            p.vy = p.vy * 0.92 + (p.hot ? 18 : -6) * dt;
            p.age += dt;
            if (p.age >= p.life) continue;

            // Color picked along PALETTE by normalized age. Hot particles
            // shift across the full ramp; smoke stays in the cool tail.
            const u = p.age / p.life;
            const idx = p.hot
                ? Math.min(state.PALETTE.length - 1, Math.floor(u * 5))
                : 4 + Math.floor(u * 1.999);
            ctx.fillStyle = state.PALETTE[idx];

            // Hot sparks are 1px; smoke draws as 2px clusters once aged.
            if (p.hot) {
                brush.px(ctx, p.x, p.y);
            } else {
                const s = u < 0.5 ? 1 : 2;
                ctx.fillRect((p.x | 0) - (s >> 1), (p.y | 0) - (s >> 1), s, s);
            }
        }
    },
});
