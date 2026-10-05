// <terminal> as its own compositor layer, in the windowed frame loop:
//  * page changes do not re-record the terminal's paint;
//  * a busy terminal does not re-record the page's paint;
//  * and, logged for the record (TERMINAL_LAYER_PERF in bro.log), the frame
//    cost of a full-screen terminal redrawn every frame with synchronized
//    output (mode 2026) beside a page with some weight.
// Under BRO_TERMINAL_LAYER=0 (terminals painted inline with the page) it
// only measures: the comparison the layer is judged against.

function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}

// Frames for at least `ms` of wall time: rAF is not paced by presentation
// on every video driver (SDL's offscreen one runs it flat out), and what
// is counted here is what the frame loop actually rendered.
async function forMs(ms) {
    const end = Date.now() + ms;
    do await frames(1); while (Date.now() < end);
}

const side = document.getElementById('side');
for (let i = 0; i < 160; ++i) {
    const d = document.createElement('div');
    d.textContent = 'item ' + i + ' — some page content';
    side.appendChild(d);
}
const clock = document.getElementById('clock');
const t = document.getElementById('t');

// One synchronized full-screen redraw: every cell rewritten, in colour.
let tick = 0;
function redraw() {
    const rows = t.rows, cols = t.cols;
    let s = '\x1b[?2026h\x1b[H';
    for (let y = 0; y < rows; ++y) {
        s += '\x1b[' + (y + 1) + ';1H\x1b[38;5;' + ((y + tick) % 216 + 16) + 'm';
        let line = '';
        const base = (tick * 7 + y * 13) % 26;
        for (let x = 0; x < cols; ++x) line += String.fromCharCode(97 + (base + x) % 26);
        s += line;
    }
    s += '\x1b[0m\x1b[?2026l';
    ++tick;
    t.feed(s);
}

async function sustained(ms, pageToo) {
    const start = Date.now();
    const s0 = bro.terminal.stats();
    let n = 0, frameSum = 0, rasterSum = 0, samples = 0, lastSample = start, due = start;
    while (Date.now() - start < ms) {
        // ~60 Hz of output, whatever the display's refresh.
        if (Date.now() >= due) {
            due += 1000 / 60;
            redraw();
            if (pageToo) clock.textContent = String(n);
            ++n;
        }
        await frames(1);
        if (Date.now() - lastSample >= 500) {
            lastSample = Date.now();
            frameSum += __bro.perf.frameTime;
            rasterSum += __bro.perf.raster;
            ++samples;
        }
    }
    const s1 = bro.terminal.stats();
    const secs = (Date.now() - start) / 1000;
    return {
        fps: +(n / secs).toFixed(1),
        frameMs: samples ? +(frameSum / samples).toFixed(2) : null,
        rasterMs: samples ? +(rasterSum / samples).toFixed(2) : null,
        pageRecords: s1.pageRecords - s0.pageRecords,
        layerRecords: s1.layerRecords - s0.layerRecords,
        updates: n,
    };
}

async function run() {
    if (!bro.terminal || !bro.terminal.available) {
        skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
        return;
    }
    await forMs(200);
    const layered = bro.terminal.stats().layered;
    assert(t.cols > 60 && t.rows > 30, 'the terminal fills most of the window: ' + t.cols + 'x' + t.rows);

    if (layered) {
        // Page changes only: the terminal's layer is left alone.
        t.feed('idle');
        await forMs(200);
        const a0 = bro.terminal.stats();
        for (let i = 0; i < 30; ++i) {
            clock.textContent = 'page ' + i;
            await forMs(20);
        }
        const a1 = bro.terminal.stats();
        assert(a1.pageRecords - a0.pageRecords >= 5, 'the page re-records as it changes: ' +
               (a1.pageRecords - a0.pageRecords));
        assert(a1.layerRecords === a0.layerRecords, 'page changes do not re-record the terminal: ' +
               a0.layerRecords + ' -> ' + a1.layerRecords);

        // Terminal output only: the page is not re-recorded.
        const out = await sustained(1500, false);
        assert(out.layerRecords >= out.updates / 3, 'the terminal re-records as it changes: ' + JSON.stringify(out));
        assert(out.pageRecords <= 1, 'terminal output does not re-record the page: ' + JSON.stringify(out));
    }

    // The measured workload: terminal output every frame, plus a page change.
    await sustained(1000, false);  // warm up
    const termOnly = await sustained(3000, false);
    const both = await sustained(3000, true);
    console.log('TERMINAL_LAYER_PERF ' + JSON.stringify({ layered, cols: t.cols, rows: t.rows, termOnly, both }));
    if (!layered) skipTest('BRO_TERMINAL_LAYER=0: measured only');
}

const watchdog = setTimeout(() => {
    assert(false, 'windowed terminal layer test timed out');
    window.close();
}, 60000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
