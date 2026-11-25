// Frame timing for the real-world scenes: rw.js's `time`, `sweep`, `perf`, and `report` commands.

import {
  AOT_BUILD_S, BRO_ROOT, BUILD, SCENES, SIZES, WORK,
  parseArgs, roundN, runBro, rwLines, splitLines, windowed,
} from './rw_core.js';

const KNEE_MS = 1000.0 / 60.0;
const TRACE_PHASES = [
  'total', 'events', 'control', 'misc', 'input', 'tick', 'js', 'layoutWait', 'record',
  'composite', 'present', 'gpuWait', 'flipWait', 'pacingWait', 'gc', 'other',
];
const MODES = {
  chromium: ['chromium', false],
  bro: ['bro', false],
  'bro-aot': ['bro', true],
};

function median(vals) {
  const s = vals.slice().sort((a, b) => a - b);
  const m = Math.floor(s.length / 2);
  return s.length % 2 === 1 ? s[m] : (s[m - 1] + s[m]) / 2;
}

function pct(xs, p) {
  const v = xs.filter(x => x !== null && x !== undefined).sort((a, b) => a - b);
  if (v.length === 0) return null;
  const k = Math.max(0, Math.min(v.length - 1, Math.round((p / 100.0) * v.length + 0.5) - 1));
  return v[k];
}

function summary(xs) {
  const v = (xs || []).filter(x => x !== null && x !== undefined);
  if (v.length === 0) return null;
  let sum = 0, mn = v[0], mx = v[0];
  for (const x of v) {
    sum += x;
    if (x < mn) mn = x;
    if (x > mx) mx = x;
  }
  return {
    median: roundN(median(v), 4),
    p95: roundN(pct(v, 95), 4),
    p99: roundN(pct(v, 99), 4),
    mean: roundN(sum / v.length, 4),
    min: roundN(mn, 4),
    max: roundN(mx, 4),
    count: v.length,
  };
}

function traceSummary(path) {
  let doc;
  try {
    doc = JSON.parse(Bronze.fs.readText(path));
  } catch (_) {
    return null;
  }
  const frames = doc && typeof doc === 'object' ? doc.frames : null;
  if (!Array.isArray(frames) || frames.length === 0) return null;
  const out = { frames: frames.length, refreshMs: doc.refreshMs };
  for (const ph of TRACE_PHASES) {
    const vals = frames.map(f => f[ph]).filter(v => typeof v === 'number');
    if (vals.length > 0) out[ph] = summary(vals);
  }
  const pres = frames.filter(f => f.presented && f.presentAt).map(f => f.presentAt).sort((a, b) => a - b);
  out.presented = pres.length;
  if (pres.length > 1) {
    out.presentsPerSec = roundN(((pres.length - 1) * 1000.0) / (pres[pres.length - 1] - pres[0]), 2);
  }
  return out;
}

function runTime(mode, scene, n, warm = 120, meas = 300, gpu = 1, perfData = '', tag = '') {
  const [browser, aot] = MODES[mode];
  const page = `${scene}.html`;
  const cfg = { mode: 'time', n, warm, meas, gpu, watchdog: 600000 };
  const name = `time_${mode}_${scene}`;
  const t0 = performance.now();
  const r = browser === 'bro'
    ? runBro(page, cfg, name, aot, null, perfData)
    : windowed('chromium', page, cfg, name);
  const wall = (performance.now() - t0) / 1000.0;
  const lines = rwLines(r.log.join('\n'));
  const timingEntry = lines.find(([t]) => t === 'timing');
  const timing = timingEntry ? timingEntry[1] : null;
  const errors = lines.filter(([t]) => t === 'error').map(([, o]) => o);
  const res = { mode, scene, n, status: r.status, wall_s: roundN(wall, 2), tag };
  if (errors.length > 0) res.errors = errors.slice(0, 4);
  const shader = r.log
    .filter(l => l.includes('Shader Error') || l.includes('Program Info Log'))
    .map(l => l.trim().slice(0, 300));
  if (shader.length > 0) res.shader_error = shader.slice(0, 3);
  if (!timing || typeof timing !== 'object') {
    res.ok = false;
    res.log_tail = r.log.filter(l => l.includes('rror')).slice(-6);
    return res;
  }
  res.ok = r.status === 'ok' && !res.shader_error;
  res.raw = {
    interval: timing.interval,
    raf: timing.raf,
    cpu: timing.cpu,
    gpu: timing.gpu,
    done: timing.done,
  };
  res.raf_ms = summary(timing.raf);
  res.interval_ms = summary(timing.interval);
  res.cpu_ms = summary(timing.cpu);
  res.gpu_ms = timing.gpu ? summary(timing.gpu) : null;
  res.delivered_ms = timing.delivered ?? null;
  res.done_ms = summary(timing.done);
  const iv = res.interval_ms ? res.interval_ms.median : null;
  const d = res.delivered_ms;
  res.frame_ms = (iv !== null && d !== null) ? roundN(Math.max(iv, d), 4) : iv;
  res.meta = {};
  for (const k of ['rafSpan', 'drainFrames', 'clock', 'gpuExt', 'disjoint', 'unresolved', 'focus', 'traceWindow', 'inner', 'ua']) {
    res.meta[k] = timing[k];
  }
  if (browser === 'bro') {
    const tr = Bronze.path.join(r.out, 'trace.json');
    if (Bronze.fs.exists(tr)) {
      res.trace = traceSummary(tr);
      res.trace_path = tr;
    }
  }
  return res;
}

function perfGroup(dso, sym) {
  const d = Bronze.path.basename(dso || '');
  const s = sym || '';
  if (d.startsWith('[JIT]') || d.startsWith('perf-') || d.startsWith('jitted-') ||
      Bronze.path.basename(Bronze.path.dirname(dso || '')).endsWith('.aot')) {
    return 'js-compiled';
  }
  if (d.includes('bronze_runtime') || d.includes('libbronze')) {
    return (s.startsWith('brass::') && !s.startsWith('brass::math::') && !s.startsWith('brass::rt'))
      ? 'jit-compiler' : 'bronze-runtime';
  }
  if (d.includes('vulkan_radeon') || d.startsWith('libvulkan') || d.startsWith('libdrm') ||
      d.startsWith('libVkLayer') || s.toLowerCase().includes('radv')) {
    return 'vulkan-driver';
  }
  if (d.startsWith('libSDL3') || d.startsWith('libwayland') || d.startsWith('libxkb') ||
      d.startsWith('libEGL') || d.startsWith('libdecor')) {
    return 'window-system';
  }
  if (d.startsWith('libc.so') || d.startsWith('libm.so') || d.startsWith('libstdc++') ||
      d.startsWith('libgcc') || d.startsWith('ld-linux')) {
    return 'libc';
  }
  if (d === 'libbro.so' || d.startsWith('bro')) {
    if (s.includes('bro::webgl')) return 'bro-webgl';
    if (s.includes('bro::bronze_host')) return 'bro-bindings';
    if (s.includes('brass::') && !s.includes('brass::math::')) return 'jit-compiler';
    if (s.includes('bronze::') || s.startsWith('bronze_') || s.startsWith('brass_') || s.startsWith('bz_')) {
      return 'bronze-runtime';
    }
    const firstNs = s.split('::')[0];
    if (s.startsWith('Sk') || s.startsWith('Gr') || s.startsWith('sk_') || s.startsWith('skgpu') ||
        s.startsWith('skia') || s.includes('skgpu::') || firstNs.includes('Sk')) {
      return 'skia';
    }
    if (s.includes('bro::render') || s.includes('bro::compositor') || s.includes('Presenter')) {
      return 'compositor-present';
    }
    if (s.includes('glslang') || s.toLowerCase().includes('spv')) return 'shader-compile';
    if (s.includes('bro::engine') || s.includes('bro::platform')) return 'bro-engine';
    if (s.includes('htmlayout') || s.includes('bro::layout') || s.includes('bro::dom')) return 'bro-layout-dom';
    return 'bro-other';
  }
  if (d === '[unknown]' || d === '') return 'unknown';
  return 'other:' + d;
}

function perfReport(data, t0Ms, t1Ms) {
  const cmd = [
    'perf', 'report', '-i', data, '--stdio', '--no-children', '-t', '\t',
    '-F', 'sample,pid,dso,sym', '--percent-limit', '0',
    `--time=${(t0Ms / 1000.0).toFixed(6)},${(t1Ms / 1000.0).toFixed(6)}`,
  ];
  const res = Bronze.exec(cmd, { timeoutMs: 300000 });
  const rows = [];
  for (const line of splitLines(res.stdout)) {
    if (!line.trim() || line.startsWith('#')) continue;
    const parts = line.split('\t');
    if (parts.length < 4) continue;
    const n = parseInt(parts[0].trim(), 10);
    if (Number.isNaN(n)) continue;
    const comm = parts[1].trim();
    const dso = parts[2].trim();
    let sym = parts[3].trim();
    if (sym.startsWith('[.] ') || sym.startsWith('[k] ')) sym = sym.slice(4);
    rows.push({ samples: n, thread: comm, dso, sym, group: perfGroup(dso, sym) });
  }
  const tids = rows.map(r => r.thread.split(':')[0]).filter(t => /^\d+$/.test(t)).map(t => parseInt(t, 10));
  const mainTid = tids.length > 0 ? Math.min(...tids) : null;
  for (const r of rows) {
    const tid = r.thread.split(':')[0];
    r.main = /^\d+$/.test(tid) && parseInt(tid, 10) === mainTid;
  }
  const total = rows.reduce((a, r) => a + r.samples, 0) || 1;
  const groups = new Map();
  const main = new Map();
  const threads = new Map();
  for (const r of rows) {
    groups.set(r.group, (groups.get(r.group) || 0) + r.samples);
    if (r.main) main.set(r.group, (main.get(r.group) || 0) + r.samples);
    if (!threads.has(r.thread)) threads.set(r.thread, new Map());
    const tm = threads.get(r.thread);
    tm.set(r.group, (tm.get(r.group) || 0) + r.samples);
  }
  const pctMap = (m, denom = total) => {
    const obj = {};
    for (const [k, v] of Array.from(m.entries()).sort((a, b) => b[1] - a[1])) {
      obj[k] = roundN((100.0 * v) / denom, 2);
    }
    return obj;
  };
  let mainSum = 0;
  for (const v of main.values()) mainSum += v;
  const threadsObj = {};
  const sortedThreads = Array.from(threads.entries()).sort((a, b) => {
    let sa = 0, sb = 0;
    for (const v of a[1].values()) sa += v;
    for (const v of b[1].values()) sb += v;
    return sb - sa;
  });
  for (const [k, m] of sortedThreads) threadsObj[k] = pctMap(m);
  rows.sort((a, b) => b.samples - a.samples);
  return {
    samples: total,
    groups_pct: pctMap(groups),
    main_thread_pct: pctMap(main),
    main_thread_share_pct: roundN((100.0 * mainSum) / total, 2),
    main_thread_norm_pct: pctMap(main, mainSum || 1),
    threads_pct: threadsObj,
    top: rows.slice(0, 60).map(r => Object.assign({}, r, { pct: roundN((100.0 * r.samples) / total, 2) })),
    stderr: res.status ? res.stderr.slice(-400) : '',
  };
}

function runPerf(scene, n, aot, outDir, meas = 600) {
  Bronze.fs.mkdir(outDir);
  const mode = aot ? 'bro-aot' : 'bro';
  const data = Bronze.path.join(outDir, `perf_${mode}_${scene}_${n}.data`);
  const startedMs = Date.now();
  const res = runTime(mode, scene, n, 120, meas, 1, data, 'perf');
  const win = res.meta && res.meta.traceWindow;
  if (res.ok && Array.isArray(win) && win[0] != null && win[1] != null) {
    res.perf = perfReport(data, win[0], win[1]);
  } else {
    res.perf = null;
  }
  delete res.raw;
  Bronze.fs.unlink(data);
  try {
    for (const f of Bronze.fs.listDir('/tmp')) {
      if (f.startsWith('perf-') && f.endsWith('.map')) {
        const p = '/tmp/' + f;
        const st = Bronze.fs.stat(p);
        if (st && st.mtimeMs >= startedMs - 1000) Bronze.fs.unlink(p);
      }
    }
  } catch (_) {}
  return res;
}

const STEP = 1.5;
const MAX_STEPS = 26;
const N_CAP_FACTOR = 20;
const BRO_MAX_FRAME_MS = 1500.0;
const BRO_RUN_BUDGET_S = 14.0;

function framesFor(predMs) {
  if (!predMs || predMs <= 0) return [120, 300];
  const warm = predMs * 120 <= 4000 ? 120 : Math.max(30, Math.floor(4000 / predMs));
  const meas = predMs * 300 <= BRO_RUN_BUDGET_S * 1000 ? 300 : Math.max(60, Math.floor((BRO_RUN_BUDGET_S * 1000) / predMs));
  return [warm, meas];
}

function restAfter(res, rest, lightRest) {
  const iv = res.frame_ms;
  const g = res.gpu_ms ? res.gpu_ms.median : null;
  const heavy = (iv && g && g / iv > 0.3) || (iv != null && iv < 2.0);
  if (heavy) return Math.min(rest, Math.max(lightRest, res.wall_s ?? rest));
  return lightRest;
}

function pointValue(runs) {
  const ok = runs.filter(r => r.ok && r.interval_ms);
  if (ok.length === 0) return null;
  const med = xs => (xs.length > 0 ? roundN(median(xs), 4) : null);
  const out = {
    frame_ms: med(ok.map(r => r.frame_ms).filter(v => v != null)),
    delivered_ms: med(ok.map(r => r.delivered_ms).filter(v => v != null)),
  };
  for (const metric of ['interval_ms', 'cpu_ms', 'gpu_ms', 'raf_ms']) {
    const rs = ok.map(r => r[metric]).filter(Boolean);
    if (rs.length > 0) {
      out[metric] = {};
      for (const k of ['median', 'p95', 'p99', 'mean']) out[metric][k] = med(rs.map(x => x[k]));
    }
  }
  const srt = ok.slice().sort((a, b) => (a.frame_ms ?? a.interval_ms.median) - (b.frame_ms ?? b.interval_ms.median));
  const rep = srt[Math.floor(srt.length / 2)];
  out.rep_run = rep.run_id;
  if (rep.trace) out.trace = rep.trace;
  return out;
}

function spread(runs) {
  const m = runs.filter(r => r.ok && r.interval_ms).map(r => r.frame_ms ?? r.interval_ms.median);
  if (m.length < 2) return null;
  const mn = Math.min(...m);
  const mx = Math.max(...m);
  return mn > 0 ? (mx - mn) / mn : null;
}

function knee(points, limit = KNEE_MS) {
  const s = points.filter(p => p.value && p.value.frame_ms).map(p => [p.n, p.value.frame_ms]);
  const over = s.findIndex(([, v]) => v > limit);
  if (over < 0) {
    return {
      first_over: null,
      last_under: s.length > 0 ? s[s.length - 1][0] : null,
      crossing_n: null,
      note: `never passed ${limit.toFixed(2)} ms in the swept range`,
    };
  }
  if (over === 0) {
    return {
      first_over: s[0][0],
      last_under: null,
      crossing_n: null,
      note: `already over ${limit.toFixed(2)} ms at the smallest N`,
    };
  }
  const [n0, v0] = s[over - 1];
  const [n1, v1] = s[over];
  let cross = n1;
  if (v0 > 0 && v1 > v0) {
    const t = (Math.log(limit) - Math.log(v0)) / (Math.log(v1) - Math.log(v0));
    cross = Math.exp(Math.log(n0) + t * (Math.log(n1) - Math.log(n0)));
  }
  return { first_over: n1, last_under: n0, crossing_n: Math.round(cross) };
}

function atKnee(sc, modes, ck) {
  const n = ck.first_over;
  if (n == null) return null;
  const vals = {};
  for (const m of modes) {
    const p = (sc.points[m] || []).find(pp => pp.n === n && pp.value);
    if (p) {
      vals[m] = {
        frame_ms: p.value.frame_ms,
        delivered_ms: p.value.delivered_ms,
        interval_ms: p.value.interval_ms,
        cpu_ms: p.value.cpu_ms,
        gpu_ms: p.value.gpu_ms,
        trace: p.value.trace,
      };
    }
  }
  const out = { n, values: vals, ratio_frame: {}, ratio_cpu: {} };
  const c = vals.chromium;
  for (const m of modes) {
    if (m === 'chromium' || !vals[m] || !c) continue;
    out.ratio_frame[m] = roundN(vals[m].frame_ms / c.frame_ms, 3);
    if (vals[m].cpu_ms && c.cpu_ms && c.cpu_ms.median) {
      out.ratio_cpu[m] = roundN(vals[m].cpu_ms.median / c.cpu_ms.median, 3);
    }
  }
  return out;
}

function machineInfo() {
  const info = {};
  try {
    info.host = Bronze.exec(['uname', '-n']).stdout.trim();
    info.kernel = Bronze.exec(['uname', '-r']).stdout.trim();
  } catch (_) {}
  try {
    info.chromium = Bronze.exec(['chromium', '--version']).stdout.trim();
  } catch (_) {}
  try {
    info.bro_commit = Bronze.exec(['git', '-C', BRO_ROOT, 'rev-parse', '--short', 'HEAD']).stdout.trim();
  } catch (_) {}
  try {
    const r = Bronze.exec([Bronze.path.join(BUILD, 'bro-ctl'), '-s', 'display', 'info']);
    const d = JSON.parse(r.stdout);
    info.display = { refreshMs: d.refreshMs, size: d.viewport, compositor: d.app, mode: d.mode };
  } catch (_) {}
  try {
    const line = splitLines(Bronze.fs.readText('/proc/cpuinfo')).find(l => l.startsWith('model name'));
    info.cpu = line ? line.split(':')[1].trim() : '';
  } catch (_) {
    info.cpu = '';
  }
  info.gpu = 'AMD Radeon 8060S (RADV in bro, radeonsi via ANGLE GL ES in Chromium)';
  return info;
}

function fmt(v, nd = 2) {
  return v == null ? '-' : Number(v).toFixed(nd);
}

function reportMd(doc) {
  const L = ['# Real-world scenes: frame timing, bro vs Chromium', ''];
  const m = doc.machine || {};
  L.push(`Machine: ${m.host} (${m.cpu}; ${m.gpu}), display ${JSON.stringify(m.display)}, ${m.chromium}, bro ${m.bro_commit}.`);
  L.push('');
  L.push('frame = max(median rAF interval, delivered), where delivered is the timed frames\' GPU-completion span per frame; cpu = the rAF callback\'s median wall time; ms.');
  L.push('');
  for (const [scene, sc] of Object.entries(doc.scenes || {})) {
    const modes = Object.keys(sc.points || {});
    L.push(`## ${scene}`, '');
    L.push('| N | ' + modes.map(mo => `${mo} frame | ${mo} cpu | ${mo} gpu`).join(' | ') + ' |');
    L.push('|' + '---|'.repeat(1 + 3 * modes.length));
    const by = {};
    for (const mo of modes) {
      by[mo] = new Map();
      for (const p of sc.points[mo]) by[mo].set(p.n, p);
    }
    for (const n of sc.n_values || []) {
      const cells = [];
      for (const mo of modes) {
        const p = by[mo].get(n) || {};
        const v = p.value || {};
        if (p.skipped) {
          cells.push('skip', '', '');
        } else {
          cells.push(fmt(v.frame_ms), fmt(v.cpu_ms && v.cpu_ms.median), fmt(v.gpu_ms && v.gpu_ms.median));
        }
      }
      L.push(`| ${n} | ${cells.join(' | ')} |`);
    }
    L.push('');
    const kn = sc.knees || {};
    L.push('60 fps knee (first N over 16.7 ms / interpolated crossing): ' + Object.entries(kn).map(([mo, k]) =>
      `${mo} ${k.first_over} / ${k.crossing_n}${k.note ? ` (${k.note})` : ''}`).join('; '));
    if (sc.stopped) L.push('', 'Sweep stopped: ' + sc.stopped);
    const ak = sc.at_chromium_knee;
    if (ak) {
      L.push('');
      L.push(`At Chromium's knee N=${ak.n}: frame ratio bro/Chromium ` +
        Object.entries(ak.ratio_frame).map(([mo, r]) => `${mo} ${r}x`).join(', ') +
        '; callback CPU ratio ' +
        Object.entries(ak.ratio_cpu).map(([mo, r]) => `${mo} ${r}x`).join(', ') + '.');
      for (const [mo, v] of Object.entries(ak.values || {})) {
        const tr = v.trace;
        if (!tr) continue;
        const g = k => tr[k] && tr[k].median;
        L.push(`- ${mo} flight recorder (median ms): total ${fmt(g('total'))}, input ${fmt(g('input'))}, ` +
          `tick ${fmt(g('tick'))} (js ${fmt(g('js'))}), composite ${fmt(g('composite'))}, present ${fmt(g('present'))}, ` +
          `gc ${fmt(g('gc'))}; presents/s ${tr.presentsPerSec}`);
      }
    }
    if (sc.perf_at_knee) {
      L.push('');
      for (const [mo, r] of Object.entries(sc.perf_at_knee)) {
        const p = r && r.perf;
        if (!p) {
          L.push(`- perf ${mo}: no profile (${r && r.status})`);
          continue;
        }
        const g = Object.entries(p.main_thread_norm_pct || {}).slice(0, 9).map(([k, v]) => `${k} ${v}%`).join(', ');
        L.push(`- perf ${mo}, main thread (${p.main_thread_share_pct}% of all samples): ${g}`);
        const o = Object.entries(p.groups_pct || {}).slice(0, 8).map(([k, v]) => `${k} ${v}%`).join(', ');
        L.push(`  all threads: ${o}`);
        const top = (p.top || []).filter(t => t.main).slice(0, 8).map(t => `${t.sym.slice(0, 60)} ${t.pct}%`).join('; ');
        L.push(`  top main-thread symbols: ${top}`);
      }
    }
    L.push('');
  }
  return L.join('\n') + '\n';
}

export function cmdTime(argv) {
  const { opts } = parseArgs(argv, {
    browser: { default: '' },
    scene: { default: '' },
    n: { type: 'int', default: 0 },
    warm: { type: 'int', default: 120 },
    meas: { type: 'int', default: 300 },
    gpu: { type: 'int', default: 1 },
    json: { default: '' },
  });
  if (!MODES[opts.browser] || !opts.scene || !opts.n) {
    console.error('usage: rw time --browser chromium|bro|bro-aot --scene S --n N');
    Bronze.exit(2);
  }
  const res = runTime(opts.browser, opts.scene, opts.n, opts.warm, opts.meas, opts.gpu);
  const brief = Object.assign({}, res);
  delete brief.raw;
  console.log(JSON.stringify(brief, null, 1));
  if (opts.json) Bronze.fs.writeText(opts.json, JSON.stringify(res, null, 1));
}

export function cmdPerf(argv) {
  const { opts } = parseArgs(argv, {
    scene: { default: '' },
    n: { type: 'int', default: 0 },
    aot: { bool: true, default: false },
    meas: { type: 'int', default: 600 },
    out: { default: Bronze.path.join(WORK, 'perf') },
    json: { default: '' },
  });
  if (!opts.scene || !opts.n) {
    console.error('usage: rw perf --scene S --n N [--aot] [--meas 600] [--out DIR]');
    Bronze.exit(2);
  }
  const res = runPerf(opts.scene, opts.n, opts.aot, opts.out, opts.meas);
  const p = res.perf || {};
  console.log(JSON.stringify({
    mode: res.mode, scene: res.scene, n: res.n, status: res.status,
    interval_ms: res.interval_ms, cpu_ms: res.cpu_ms, gpu_ms: res.gpu_ms,
  }, null, 1));
  console.log('groups %:', JSON.stringify(p.groups_pct, null, 1));
  console.log(`main thread % of all samples: ${p.main_thread_share_pct}; its groups:`, JSON.stringify(p.main_thread_pct, null, 1));
  const tSums = {};
  for (const [k, v] of Object.entries(p.threads_pct || {})) {
    tSums[k] = Object.values(v).reduce((a, b) => a + b, 0);
  }
  console.log('threads %:', JSON.stringify(tSums, null, 1));
  for (const r of (p.top || []).slice(0, 30)) {
    console.log(
      `  ${r.pct.toFixed(2).padStart(6)}%  ${r.group.padEnd(18)} ${r.thread.slice(0, 14).padEnd(14)} ` +
      `${Bronze.path.basename(r.dso).slice(0, 24).padEnd(24)} ${r.sym.slice(0, 90)}`
    );
  }
  if (opts.json) Bronze.fs.writeText(opts.json, JSON.stringify(res, null, 1));
}

export function cmdSweep(argv) {
  const { opts } = parseArgs(argv, {
    scenes: { default: '' },
    modes: { default: 'chromium,bro,bro-aot' },
    out: { default: Bronze.path.join(WORK, 'timing') },
    rest: { type: 'float', default: 20.0 },
    'light-rest': { type: 'float', default: 5.0 },
    reps: { type: 'int', default: 2 },
    perf: { type: 'int', default: 1 },
    merge: { bool: true, default: false },
    reuse: { bool: true, default: false },
  });
  const scenes = opts.scenes ? opts.scenes.split(',') : SCENES;
  const modes = opts.modes.split(',');
  const outDir = opts.out;
  Bronze.fs.mkdir(outDir);
  const raw = {};
  const doc = {
    schema: 'bro-realworld-timing/1',
    units: 'ms',
    machine: machineInfo(),
    scenes: {},
  };
  const prevPath = Bronze.path.join(outDir, 'timing.json');
  if (opts.merge && Bronze.fs.exists(prevPath)) {
    doc.scenes = JSON.parse(Bronze.fs.readText(prevPath)).scenes || {};
    const rawPath = Bronze.path.join(outDir, 'timing_raw.json');
    if (Bronze.fs.exists(rawPath)) Object.assign(raw, JSON.parse(Bronze.fs.readText(rawPath)));
  }

  function save(isFinal = false) {
    Bronze.fs.writeText(Bronze.path.join(outDir, isFinal ? 'timing.json' : 'sweep.partial.json'), JSON.stringify(doc, null, 1));
    Bronze.fs.writeText(Bronze.path.join(outDir, 'timing_raw.json'), JSON.stringify(raw));
  }

  function runPoint(mode, scene, n, warm = 120, meas = 300) {
    const runs = [];
    for (let i = 0; i <= opts.reps; i++) {
      if (i === opts.reps) {
        const sp = spread(runs);
        if (sp === null || sp <= 0.10) break;
      }
      const res = runTime(mode, scene, n, warm, meas);
      res.run_id = `${scene}/${mode}/${n}/${runs.length}`;
      raw[res.run_id] = res.raw || null;
      delete res.raw;
      res.warm = warm;
      res.meas = meas;
      runs.push(res);
      const iv = res.interval_ms ? res.interval_ms.median : null;
      console.log(
        `  ${mode.padEnd(9)} ${scene.padEnd(16)} N=${String(n).padEnd(8)} run ${runs.length}: ` +
        `frame ${res.frame_ms} ms  raf-interval ${iv}  delivered ${res.delivered_ms}  ` +
        `cpu ${res.cpu_ms && res.cpu_ms.median}  gpu ${res.gpu_ms && res.gpu_ms.median}  status ${res.status}  (${res.wall_s} s)`
      );
      if (!res.ok) break;
      Bronze.sleep(Math.round(restAfter(res, opts.rest, opts['light-rest']) * 1000));
    }
    return { n, runs, value: pointValue(runs), spread: spread(runs) };
  }

  const t0 = performance.now();
  for (const scene of scenes) {
    console.log(`== ${scene}`);
    if (!opts.reuse) delete doc.scenes[scene];
    const sc = doc.scenes[scene] || (doc.scenes[scene] = { points: {} });
    for (const m of modes) sc.points[m] = [];

    if (!opts.reuse || !sc.n_values) {
      const ns = [];
      let past = -1;
      let n = SIZES[scene][0];
      while (ns.length < MAX_STEPS) {
        const p = runPoint('chromium', scene, n);
        sc.points.chromium.push(p);
        save();
        if (!p.value) {
          const r0 = p.runs[0] || {};
          sc.stopped = `Chromium failed at N=${n}: ` + String(r0.shader_error || r0.errors || r0.status).slice(0, 300);
          break;
        }
        ns.push(n);
        if (past < 0 && p.value.frame_ms > KNEE_MS) past = 0;
        else if (past >= 0) past += 1;
        if (past >= 2) break;
        n = Math.max(n + 1, Math.round(n * STEP));
        if (n > N_CAP_FACTOR * SIZES[scene][1]) {
          sc.stopped = `N cap (${N_CAP_FACTOR} x the planned max ${SIZES[scene][1]}) reached`;
          break;
        }
      }
      sc.n_values = ns;
    }

    for (const mode of modes) {
      if (mode === 'chromium') continue;
      let last = null;
      for (const n of sc.n_values) {
        let pred;
        if (last) pred = (last[1] * n) / last[0];
        else {
          const c = (sc.points.chromium || []).find(pp => pp.n === n);
          pred = ((c && c.value && c.value.frame_ms) || 5.0) * 6;
        }
        if (pred > BRO_MAX_FRAME_MS) {
          sc.points[mode].push({ n, skipped: `predicted ${pred.toFixed(0)} ms a frame` });
          continue;
        }
        const [warm, meas] = framesFor(pred);
        const p = runPoint(mode, scene, n, warm, meas);
        sc.points[mode].push(p);
        save();
        if (p.value) last = [n, p.value.frame_ms];
      }
    }
    const allm = Object.keys(sc.points);
    sc.knees = {};
    for (const m of allm) sc.knees[m] = knee(sc.points[m]);
    if (AOT_BUILD_S.has(scene)) sc.aot_build_s = AOT_BUILD_S.get(scene);
    sc.at_chromium_knee = atKnee(sc, allm, sc.knees.chromium || {});
    save();
    console.log('   knees:', JSON.stringify(sc.knees));
    console.log('   at chromium knee:', JSON.stringify(sc.at_chromium_knee && sc.at_chromium_knee.ratio_frame));

    if (opts.perf && sc.at_chromium_knee) {
      const kn = sc.at_chromium_knee.n;
      sc.perf_at_knee = sc.perf_at_knee || {};
      for (const mode of modes) {
        if (mode === 'chromium') continue;
        const v = sc.at_chromium_knee.values[mode] && sc.at_chromium_knee.values[mode].frame_ms;
        const meas = v ? Math.max(60, Math.min(600, Math.floor(4000 / v))) : 120;
        const r = runPerf(scene, kn, mode === 'bro-aot', Bronze.path.join(outDir, 'perf'), meas);
        sc.perf_at_knee[mode] = {
          n: r.n, status: r.status, interval_ms: r.interval_ms, cpu_ms: r.cpu_ms,
          gpu_ms: r.gpu_ms, trace: r.trace, perf: r.perf,
        };
        console.log(`   perf ${mode} N=${kn}: main thread ${JSON.stringify(r.perf && r.perf.main_thread_norm_pct)}`);
        save();
        Bronze.sleep(Math.round(opts.rest * 1000));
      }
    }
  }
  doc.elapsed_s = Math.round((performance.now() - t0) / 1000.0);
  save(true);
  Bronze.fs.unlink(Bronze.path.join(outDir, 'sweep.partial.json'));
  console.log('wrote', Bronze.path.join(outDir, 'timing.json'));
}

export function cmdReport(argv) {
  const { opts } = parseArgs(argv, {
    json: { default: '' },
    out: { default: '' },
  });
  if (!opts.json) {
    console.error('usage: rw report --json DIR/timing.json [--out DIR/report.md]');
    Bronze.exit(2);
  }
  const doc = JSON.parse(Bronze.fs.readText(opts.json));
  const md = reportMd(doc);
  if (opts.out) Bronze.fs.writeText(opts.out, md);
  console.log(md);
}
