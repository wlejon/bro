// bro.worldgen stage bisection — world.stage() / world.stageSync().
//
// elevation() is the pipeline's product; stage() is the DAG that produces it,
// and the reason to have it is diagnostic. The pipeline is three nets in
// series, so when the output is wrong the only useful question is which stage
// it was already wrong in — and that question cannot be asked through an API
// that returns metres and nothing else.
//
// The binding surface is checked without weights. Everything below that needs
// a converted checkpoint runs only when one is present, so this is CI-safe:
//   BRO_TERRAIN_WEIGHTS=<dir> bro-headless tests/_smoke_app tests/worldgen/test_worldgen_stage.js

const STAGES = {
    coarse:     { channels: 6, first: 'elevation', cellMul: 256 },
    latent:     { channels: 5, first: 'latent0',   cellMul: 8   },
    latentInit: { channels: 5, first: 'latent0',   cellMul: 8   },
    residual:   { channels: 1, first: 'residual',  cellMul: 1   },
    elevation:  { channels: 1, first: 'elevation', cellMul: 1   },
};

assert(typeof bro === 'object', 'bro global exists');
assert(bro.worldgen !== undefined && bro.worldgen !== null,
       'bro.worldgen namespace exists');

if (bro.worldgen.available === false) {
    let err = null;
    try { bro.worldgen.loadWorld('x'); } catch (e) { err = e; }
    assert(err !== null, 'stub loadWorld() throws');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.worldgen is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadWorld']) {
        assert(typeof bro.worldgen[f] === 'function', `bro.worldgen.${f} is a function`);
    }

    const dir = (typeof process === 'object' && process.env
                 && process.env.BRO_TERRAIN_WEIGHTS)
              || 'D:/projects/brodiffusion/weights/terrain-diffusion-30m-bro';

    let exists = false;
    try {
        const fs = require('fs');
        exists = fs.existsSync(dir + '/config.json');
    } catch (e) { exists = false; }

    if (!exists) {
        console.log('no converted checkpoint at ' + dir +
                    ' — skipping the weights-backed stage checks');
    } else {
        bro.worldgen.init();

        let world = null, loadErr = null;
        bro.worldgen.loadWorld(dir, {
            seed: 42,
            onReady: (w) => { world = w; },
            onError: (m) => { loadErr = m; },
        });
        // Predicate is ready-OR-error, so a failure exits now instead of
        // burning the whole budget.
        for (let i = 0; i < 600 && !world && !loadErr; i++) { sleep(100); flush(); }
        assert(!loadErr, 'world loaded without error: ' + loadErr);
        assert(world, 'world loaded within budget');

        const native = world.cellSize;
        assert(native > 0, `cellSize is a real number (${native})`);
        assert(Math.abs(world.latentCellSize - native * 8) < 1e-6,
            `latentCellSize is 8 native cells (${world.latentCellSize})`);
        assert(Math.abs(world.coarseCellSize - native * 256) < 1e-6,
            `coarseCellSize is 256 native cells (${world.coarseCellSize})`);

        // A small window: cost is dominated by fixed overhead, and every
        // property below is about shape and domain rather than extent.
        const N = 64;
        const results = {};
        for (const [name, want] of Object.entries(STAGES)) {
            const r = world.stageSync(name, 0, 0, N, N);
            results[name] = r;

            assert(r.stage === name, `${name}: echoes its own name`);
            assert(r.channels === want.channels,
                `${name}: ${want.channels} channels (got ${r.channels})`);
            assert(r.names.length === want.channels,
                `${name}: one name per channel`);
            assert(r.units.length === want.channels,
                `${name}: one unit per channel`);
            assert(r.names[0] === want.first,
                `${name}: channel 0 is ${want.first} (got ${r.names[0]})`);
            assert(r.data.length === r.channels * r.width * r.height,
                `${name}: data is channels*w*h planar (${r.data.length})`);
            assert(Math.abs(r.cellSize - native * want.cellMul) < 1e-6,
                `${name}: cellSize is ${want.cellMul} native cells (${r.cellSize})`);

            // Nothing may come back as NaN — a stage that silently produces
            // non-numbers renders as a plausible black square.
            let bad = 0;
            for (let i = 0; i < r.data.length; i += 7)
                if (!isFinite(r.data[i])) bad++;
            assert(bad === 0, `${name}: no non-finite samples (${bad})`);
        }

        // Domains. These are the checks that catch a channel being handed over
        // in the wrong transform — the failure mode that renders perfectly.
        const elevCh = (r, c) => {
            const plane = r.width * r.height;
            let lo = Infinity, hi = -Infinity;
            for (let i = 0; i < plane; i++) {
                const v = r.data[c * plane + i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            return [lo, hi];
        };

        // The checkpoint is documented as roughly -1550 m ocean floor to
        // +2850 m peaks. A generous bound still catches the ~1550x inflation
        // that applying coarse_means/coarse_stds a second time would produce.
        const [cLo, cHi] = elevCh(results.coarse, 0);
        assert(cLo > -20000 && cHi < 20000,
            `coarse elevation is metres, not a normalised domain (${cLo.toFixed(0)}..${cHi.toFixed(0)})`);
        const [eLo, eHi] = elevCh(results.elevation, 0);
        assert(eLo > -20000 && eHi < 20000,
            `elevation is metres (${eLo.toFixed(0)}..${eHi.toFixed(0)})`);
        const [lLo, lHi] = elevCh(results.latent, 4);
        assert(lLo > -20000 && lHi < 20000,
            `latent lowFrequency is metres (${lLo.toFixed(0)}..${lHi.toFixed(0)})`);

        // The residual is deliberately NOT metres — it is the network's own
        // standardised domain, and converting it needs the latent low band and
        // a denoise round-trip. Its unit string has to say so.
        assert(results.residual.units[0] === 'standardised',
            'residual is labelled standardised rather than metres');

        // latentInit is step 1 of the latent stage's 2, so it must differ from
        // the settled result. Identical output would mean the second TrigFlow
        // step is a no-op and the bisection is worthless.
        {
            const a = results.latent, b = results.latentInit;
            let diff = 0;
            for (let i = 0; i < a.data.length; i += 13)
                diff = Math.max(diff, Math.abs(a.data[i] - b.data[i]));
            assert(diff > 1e-6,
                `latentInit differs from the settled latent (max ${diff})`);
        }

        // stage('elevation') and the existing elevationSync must be the SAME
        // function. They take different paths into the pipeline, so this is
        // what stops the new surface drifting from the shipped one.
        {
            const viaStage = results.elevation;
            const viaElev = world.elevationSync(0, 0, N, N, { margin: 0 });
            assert(viaElev.width === viaStage.width &&
                   viaElev.height === viaStage.height,
                'stage and elevationSync agree on shape');
            let maxDelta = 0;
            for (let i = 0; i < viaStage.data.length; i += 11)
                maxDelta = Math.max(maxDelta,
                    Math.abs(viaStage.data[i] - viaElev.data[i]));
            assert(maxDelta === 0,
                `stage('elevation') is elevationSync exactly (maxDelta ${maxDelta})`);
        }

        // The async form is what a frame loop uses, so it has to actually
        // deliver rather than merely not throw.
        {
            let got = null, asyncErr = null;
            world.stage('coarse', 0, 0, N, N, {
                onDone: (r) => { got = r; },
                onError: (m) => { asyncErr = m; },
            });
            for (let i = 0; i < 600 && !got && !asyncErr; i++) { sleep(100); flush(); }
            assert(!asyncErr, 'async stage() reported no error: ' + asyncErr);
            assert(got, 'async stage() delivered a result');
            assert(got.channels === 6 && got.stage === 'coarse',
                'async stage() delivers the same shape as stageSync');
        }

        // An exception thrown INSIDE an async callback must be reported through
        // the error funnel (window.onerror), not swallowed. Regression: the
        // binding's JS_Call sites checked JS_IsException and then discarded the
        // exception with no log, so a TypeError in onDone froze the caller's
        // generation chain with zero diagnostic — a one-line app bug read as an
        // unbounded hang. It must surface, and the async machinery must survive
        // it (report-and-continue), so a following request still completes.
        {
            const prevOnError = (typeof window === 'object') ? window.onerror : undefined;
            let reported = null;
            window.onerror = (message) => { reported = String(message); return false; };

            world.stage('elevation', 0, 0, N, N, {
                onDone: () => { throw new TypeError('boom from onDone'); },
            });
            for (let i = 0; i < 600 && !reported; i++) { sleep(100); flush(); }
            assert(reported, 'a throw inside onDone reached window.onerror');
            assert(reported.includes('boom from onDone'),
                'the reported error is the one that was thrown: ' + reported);

            // Report-and-continue: the machinery is not wedged, a later request
            // still lands.
            let after = null;
            world.stage('coarse', 0, 0, N, N, { onDone: (r) => { after = r; } });
            for (let i = 0; i < 600 && !after; i++) { sleep(100); flush(); }
            assert(after && after.stage === 'coarse',
                'the async pipeline survives a throwing callback');

            window.onerror = prevOnError;
        }

        // Unknown stages are refused by name rather than silently defaulting.
        {
            let err = null;
            try { world.stageSync('nope', 0, 0, 8, 8); } catch (e) { err = e; }
            assert(err !== null, 'an unknown stage name throws');
            assert(String(err.message).includes('unknown stage'),
                'the error names the problem: ' + err.message);
        }

        console.log('worldgen stage test passed');
    }
}
