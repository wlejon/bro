// Frame timing for mode=time (lib/harness.js timeLoop). The page renders
// cfg.warm frames untimed (shaders compile, JIT tiers settle), then cfg.meas
// timed ones, recording per frame:
//   interval  wall-clock time from the previous rAF callback's start to this
//             one's (ms): the frame interval
//   raf       the rAF timestamp minus the previous one (ms)
//   cpu       wall-clock time around the scene's update + render (ms)
//   gpu       TIME_ELAPSED_EXT around the same WebGL work, when
//             EXT_disjoint_timer_query_webgl2 is there (ms; null while a
//             query was disjoint)
//
// The wall clock is performance.now() in Chromium. In bro it is
// __host.wallMs() (steady_clock): bro's performance.now() is its frame clock,
// constant inside a callback and advanced to the vblank the frame is expected
// on, so neither it nor the rAF timestamp measures an uncapped frame (several
// frames inside one refresh share a timestamp).
//
// After the last timed frame the page keeps its rAF loop going without
// rendering until every GPU query has resolved (at most drainMaxMs); the
// time each frame's query is first seen available is its GPU completion,
// from which `delivered` (ms a frame, as the GPU finished them) comes. In bro the flight
// recorder's records of the timed frames are fetched with
// controlCommand('trace', ...) (docs/agent-control.md) and come back as the
// trace.json blob; the trace clock is CLOCK_MONOTONIC, which is also what
// perf samples with under `-k CLOCK_MONOTONIC`, so a profile lines up.

const drainMaxMs = 20000;

const wall = (typeof __host !== 'undefined' && __host && typeof __host.wallMs === 'function')
    ? () => __host.wallMs()
    : () => performance.now();
const wallName = (typeof __host !== 'undefined' && __host && typeof __host.wallMs === 'function') ? '__host.wallMs' : 'performance.now';

function hasFocus() {
    try {
        if (typeof bro !== 'undefined' && bro && bro.window && typeof bro.window.focused === 'boolean') return bro.window.focused;
        if (typeof document !== 'undefined' && typeof document.hasFocus === 'function') return document.hasFocus();
    } catch (e) { /* unknown */ }
    return null;
}

export function makeTimer(cfg, gl) {
    const t = {
        warm: cfg.warm, meas: cfg.meas,
        starts: [], ts: [], cpu: [], gpu: [], doneAt: [], disjoint: 0, drainStart: 0,
        ext: null, pending: [], free: [],
        ctl: typeof controlCommand === 'function' && typeof controlResult === 'function',
        ctlIds: {}, ctlRes: {}, focus: [],
        drained: 0,
    };
    if (cfg.gpu && gl) {
        try { t.ext = gl.getExtension('EXT_disjoint_timer_query_webgl2'); } catch (e) { t.ext = null; }
    }

    const pollQueries = () => {
        if (!t.ext) return;
        let disjoint = false;
        try { disjoint = !!gl.getParameter(t.ext.GPU_DISJOINT_EXT); } catch (e) { /* keep false */ }
        if (disjoint) t.disjoint++;
        while (t.pending.length) {
            const p = t.pending[0];
            if (!gl.getQueryParameter(p.q, gl.QUERY_RESULT_AVAILABLE)) break;
            const ns = gl.getQueryParameter(p.q, gl.QUERY_RESULT);
            t.gpu[p.i] = disjoint ? null : ns / 1e6;
            t.doneAt[p.i] = wall();
            t.pending.shift();
            t.free.push(p.q);
        }
    };

    const ctlIssue = (key, ...argv) => { if (t.ctl) t.ctlIds[key] = controlCommand(...argv); };
    const ctlPoll = () => {
        if (!t.ctl) return true;
        let all = true;
        for (const k of Object.keys(t.ctlIds)) {
            if (k in t.ctlRes) continue;
            const r = controlResult(t.ctlIds[k]);
            if (r === null) { all = false; continue; }
            t.ctlRes[k] = r;
        }
        return all;
    };

    return {
        // Called by the loop for each frame f with its rAF timestamp, around
        // `body` (the scene's frame). Returns false once timing and its
        // draining are over.
        frame(f, ts, body) {
            const start = wall();
            const end = t.warm + t.meas;
            const timed = f >= t.warm && f < end;
            if (f === t.warm - 1) { t.starts.push(start); t.ts.push(ts); }
            if (f === t.warm) { ctlIssue('t0', 'now'); t.focus.push(hasFocus()); }
            let q = null;
            if (timed && t.ext) {
                q = t.free.pop() || gl.createQuery();
                gl.beginQuery(t.ext.TIME_ELAPSED_EXT, q);
            }
            const c0 = wall();
            if (f < end) body();
            const c1 = wall();
            if (q) {
                gl.endQuery(t.ext.TIME_ELAPSED_EXT);
                t.pending.push({ q, i: t.cpu.length });
            }
            if (timed) {
                t.starts.push(start);
                t.ts.push(ts);
                t.cpu.push(c1 - c0);
                t.gpu.push(null);
                t.doneAt.push(null);
            }
            if (f === end - 1) { ctlIssue('t1', 'now'); t.focus.push(hasFocus()); }
            pollQueries();
            if (f < end) return true;
            if (!t.drainStart) t.drainStart = start;
            t.drained++;
            const ctlDone = ctlPoll();
            if (ctlDone && t.ctl && ('t0' in t.ctlRes) && ('t1' in t.ctlRes) && !('trace' in t.ctlIds)) {
                ctlIssue('trace', 'trace', '--since=' + t.ctlRes.t0.payload.trim(), '--until=' + t.ctlRes.t1.payload.trim(), '--json', '--frames');
                return true;
            }
            return !((t.pending.length === 0 && ctlDone) || start - t.drainStart >= drainMaxMs);
        },
        result() {
            const diff = (a) => { const o = []; for (let i = 1; i < a.length; i++) o.push(+(a[i] - a[i - 1]).toFixed(4)); return o; };
            // Delivered: the span from the first timed frame's GPU completion
            // (its query first seen available) to the last one's, per frame:
            // bounded by the GPU even where the rAF loop runs ahead of it
            // (Chromium queues frames without waiting for them), and by the
            // CPU where frames complete as fast as they are submitted.
            const dn = t.doneAt;
            const delivered = (dn.length > 1 && dn[0] !== null && dn[dn.length - 1] !== null) ? +((dn[dn.length - 1] - dn[0]) / (dn.length - 1)).toFixed(4) : null;
            const done = t.doneAt.every((x) => x !== null) ? diff(t.doneAt) : null;
            const num = (k) => (t.ctlRes[k] && t.ctlRes[k].ok) ? Number(t.ctlRes[k].payload.trim()) : null;
            return {
                interval: diff(t.starts),
                delivered, done,
                rafSpan: t.starts.length > 1 ? +(t.starts[t.starts.length - 1] - t.starts[1]).toFixed(3) : null,
                drainFrames: t.drained,
                raf: diff(t.ts),
                cpu: t.cpu.map((x) => +x.toFixed(4)),
                gpu: t.ext ? t.gpu.map((x) => (x === null ? null : +x.toFixed(4))) : null,
                clock: wallName,
                gpuExt: !!t.ext, disjoint: t.disjoint, unresolved: t.pending.length,
                focus: t.focus,
                traceWindow: t.ctl ? [num('t0'), num('t1')] : null,
                inner: typeof window !== 'undefined' ? [window.innerWidth, window.innerHeight, window.devicePixelRatio] : null,
                ua: typeof navigator !== 'undefined' ? String(navigator.userAgent) : '',
            };
        },
        trace() { return t.ctlRes.trace || null; },
    };
}
