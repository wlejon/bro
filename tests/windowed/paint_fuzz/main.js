// The paint cache fuzz, windowed: the seeded steps of steps.js on the real
// frame loop, where the base recording and its spans survive across presents.
// After each step the clock is frozen (bro.time.scale = 0: transitions and
// animations hold still mid-flight, rAF still runs) and the presented frame
// the span cache produced is compared with the one a full re-record presents
// (setPaintCache(false), the document marked so the base records again).
// presentedFrame() needs BRO_CAPTURE_PRESENTS=1, set by tests/run_tests.sh.
// Headless (PAINT_FUZZ_MODE=headless) this script does nothing: headless.js
// drives the page there.

(function () {
    if (typeof process !== 'undefined' && process.env && process.env.PAINT_FUZZ_MODE === 'headless') return;

    const seed = Number((typeof process !== 'undefined' && process.env && process.env.PAINT_FUZZ_SEED) || 7) >>> 0;
    const STEPS = 40;

    function frames(n) {
        return new Promise((resolve) => {
            const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
            requestAnimationFrame(step);
        });
    }
    // On the wall clock: timers run on the scaled one, which stands still
    // while the steps are compared.
    async function wait(ms) {
        const end = Date.now() + ms;
        while (Date.now() < end) await frames(1);
    }
    // A presented frame once presents stop changing (a rAF is not a present).
    async function settled() {
        let last = null;
        const deadline = Date.now() + 2000;
        for (;;) {
            await wait(30);
            await frames(2);
            const img = presentedFrame();
            if (img && last && PaintFuzz.same(img, last)) return img;
            last = img;
            if (Date.now() > deadline) return img;
        }
    }

    async function run() {
        const fuzz = PaintFuzz.create(seed);
        fuzz.build();
        await frames(10);
        if (!(await settled())) {
            assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 and a readable swapchain needed)');
            return;
        }
        const history = [];
        const sentinel = document.getElementById('sentinel');
        let mark = 0;
        for (let i = 0; i < STEPS; ++i) {
            const s = fuzz.step();
            history.push(i + ':' + s.what);
            if (s.dpr) continue;   // a window's scale is its display's
            if (s.hover) mouseMove(s.hover[0], s.hover[1]);
            bro.time.scale = 1;
            await wait(s.advance || 0);
            bro.time.scale = 0;
            const cached = await settled();
            setPaintCache(false);
            sentinel.style.left = (-50 - (++mark % 2)) + 'px';
            const full = await settled();
            setPaintCache(true);
            const d = PaintFuzz.diff(cached, full);
            if (d.count !== 0) {
                assert(false, `windowed paint cache fuzz seed=${seed}: step ${i} (${s.what}) — ${d.count} pixels differ ` +
                              `from a full re-record, first at ${JSON.stringify(d.first)} (cached, full); last steps: ` +
                              history.slice(-8).join(' | '));
                return;
            }
        }
        const ps = perf.stats().paint;
        console.log(`windowed paint fuzz seed=${seed}: ${history.length} steps ok, reused=${ps.reused} recorded=${ps.recorded} ` +
                    `misses=${JSON.stringify(ps.misses)}`);
        assert(ps.reused > 0, 'windowed paint cache fuzz: no span was ever reused, so nothing was tested');
    }

    const watchdog = setTimeout(() => {
        assert(false, 'windowed paint cache fuzz timed out');
        window.close();
    }, 90000);
    run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
        clearTimeout(watchdog);
        window.close();
    });
})();
