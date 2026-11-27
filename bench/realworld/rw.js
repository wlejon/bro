// Real-world three.js / pixi.js scenes: correctness and frame timing (README.md).
//
// Run directly via bronze or compile to a native executable:
//   bronze run bench/realworld/rw.js -- l1|surface|record|shot|render|time|sweep|perf|report|js|compare ...
//   bronze build bench/realworld/rw.js -o rw && ./rw l1

import {
  BRO_HEADLESS, ROOT, SCENES, SIZES, SMALL, TIERS, WORK,
  headlessAot, makeBroApp, parseArgs, rwLines, splitLines, windowed,
} from './rw_core.js';
import { compareShots, runCompareCli } from './compare_shots.js';
import { cmdPerf, cmdReport, cmdSweep, cmdTime } from './timing.js';
import { cmdJs } from './jsbench.js';

// ---- Level 1 ----------------------------------------------------------------

function runNodeState(scene, n, frames, cp) {
  const res = Bronze.exec([
    'node', Bronze.path.join(ROOT, 'scenes', `${scene}.js`),
    'mode=state', `n=${n}`, `frames=${frames}`, `cp=${cp}`,
  ], { timeoutMs: 300000 });
  return [res.status, rwLines(res.stdout), res.stderr];
}

function runHeadlessState(scene, n, frames, cp, tier) {
  const [kind, envExtra] = TIERS[tier];
  const cfg = { mode: 'state', n, frames, cp, quit: 0 };
  const d = makeBroApp(`l1_${scene}_${n}_${tier}`, `${scene}.html`, cfg);
  const env = Object.assign({ BRO_TEST_VK_VALIDATION: '0' }, envExtra);
  const res = kind === 'aot'
    ? headlessAot(d, ['--no-gpu', '-e', 'flush()'], env)
    : Bronze.exec([BRO_HEADLESS, '--grant', 'declared', d, '--no-gpu', '-e', 'flush()'], { env, timeoutMs: 300000 });
  const lines = rwLines(res.stdout);
  const errs = splitLines(res.stdout + res.stderr).filter(l => l.includes('rror') && !l.includes('RW ')).slice(0, 8);
  return [res.status, lines, errs.join('\n')];
}

function deepEqual(a, b) {
  if (a === b) return true;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i])) return false;
    return true;
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    const ka = Object.keys(a);
    const kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) if (!deepEqual(a[k], b[k])) return false;
    return true;
  }
  return false;
}

function compareStates(ref, got) {
  const rs = ref.filter(([t]) => t === 'state').map(([, o]) => o);
  const gs = got.filter(([t]) => t === 'state').map(([, o]) => o);
  if (gs.length === 0) {
    const errs = got.filter(([t]) => t === 'error').map(([, o]) => o);
    return 'no-output' + (errs.length > 0 ? ': ' + errs[0].msg : '');
  }
  if (rs.length !== gs.length) return `checkpoints ${rs.length} vs ${gs.length}`;
  let worst = 'match';
  const ulps = [];
  for (let idx = 0; idx < rs.length; idx++) {
    const r = rs[idx];
    const g = gs[idx];
    for (const k of Object.keys(r)) {
      const a = r[k];
      const b = g[k];
      if (deepEqual(a, b)) continue;
      if (k === 'sample' && Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
        let close = true;
        for (let i = 0; i < a.length; i++) {
          if (Math.abs(a[i] - b[i]) > 1e-12 * Math.max(1.0, Math.abs(a[i]))) {
            close = false;
            break;
          }
        }
        if (close) continue;
        return `MISMATCH f=${r.f} sample: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
      }
      if (typeof a === 'string' && typeof b === 'string') {
        const pa = a.split(':');
        const pb = b.split(':');
        if (pa.length === 3 && pb.length === 3 && pa[1] === pb[1] && pa[2] === pb[2]) {
          worst = 'ulp';
          ulps.push(`${k}@${r.f}`);
          continue;
        }
      }
      return `MISMATCH f=${r.f} ${k}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
    }
  }
  return worst + (ulps.length > 0 ? ' (' + ulps.slice(0, 6).join(',') + ')' : '');
}

function cmdL1(argv) {
  const { opts } = parseArgs(argv, {
    scenes: { default: '' },
    tiers: { default: 'jit,aot,deopt3,gcstress' },
    sizes: { default: 'min,max' },
    frames: { type: 'int', default: 120 },
    'json-out': { default: '' },
  });
  const scenes = opts.scenes ? opts.scenes.split(',') : SCENES;
  const tiers = opts.tiers.split(',');
  const rows = [];
  for (const scene of scenes) {
    const [lo, hi] = SIZES[scene];
    const sizes = { min: lo, max: hi, small: SMALL[scene] };
    for (const sz of opts.sizes.split(',')) {
      const n = sz in sizes ? sizes[sz] : parseInt(sz, 10);
      const frames = opts.frames;
      const cp = `0,1,2,${Math.floor(frames / 2)},${frames - 1}`;
      const [rc, ref, err] = runNodeState(scene, n, frames, cp);
      if (rc !== 0 || ref.length === 0) {
        rows.push([scene, n, 'node', `node failed rc=${rc} ${err.slice(-300)}`]);
        continue;
      }
      for (const tier of tiers) {
        if (tier === 'gcstress' && n > lo) continue;
        const f = tier === 'gcstress' ? Math.min(frames, 10) : frames;
        let cp2 = cp;
        let ref2 = ref;
        if (f !== frames) {
          cp2 = `0,1,2,${Math.floor(f / 2)},${f - 1}`;
          ref2 = runNodeState(scene, n, f, cp2)[1];
        }
        let verdict;
        try {
          const [, got, herr] = runHeadlessState(scene, n, f, cp2, tier);
          verdict = compareStates(ref2, got);
          if (!verdict.startsWith('match') && !verdict.startsWith('ulp') && herr) {
            verdict += ' | ' + herr.replace(/\n/g, ' / ').slice(0, 400);
          }
        } catch (e) {
          verdict = (`harness error: ${String(e)}`).slice(0, 600);
        }
        rows.push([scene, n, tier, verdict]);
        console.log(`${scene.padEnd(16)} N=${String(n).padEnd(7)} ${tier.padEnd(9)} ${verdict}`);
      }
    }
  }
  if (opts['json-out']) Bronze.fs.writeText(opts['json-out'], JSON.stringify(rows, null, 1));
}

// ---- Render / Surface / Record / Shot ---------------------------------------

function cmdRender(argv) {
  const { opts } = parseArgs(argv, {
    browser: { default: '' },
    scene: { default: '' },
    n: { type: 'int', default: 0 },
    frames: { type: 'int', default: 60 },
  });
  if ((opts.browser !== 'bro' && opts.browser !== 'chromium') || !opts.scene) {
    console.error('usage: rw render --browser bro|chromium --scene S [--n N] [--frames 60]');
    Bronze.exit(2);
  }
  const cfg = { mode: 'render', n: opts.n || SMALL[opts.scene], frames: opts.frames };
  const r = windowed(opts.browser, `${opts.scene}.html`, cfg, `render_${opts.browser}_${opts.scene}`);
  console.log('status', r.status);
  for (const line of r.log) {
    const i = line.indexOf('RW ');
    if (i >= 0) console.log(line.slice(i, i + 400));
  }
}

function cmdSurface(argv) {
  const { opts } = parseArgs(argv, { all: { bool: true, default: false } });
  const res = {};
  for (const browser of ['bro', 'chromium']) {
    const r = windowed(browser, 'surface.html', { mode: 'surface', w: 640, h: 480 }, `surface_${browser}`);
    const p = Bronze.path.join(r.out, 'surface.json');
    if (!Bronze.fs.exists(p)) {
      console.log(browser, 'no surface.json; status', r.status);
      console.log(r.log.filter(l => l.includes('RW') || l.includes('rror')).join('\n').slice(-2000));
      return;
    }
    res[browser] = JSON.parse(Bronze.fs.readText(p));
  }
  Bronze.fs.writeText(Bronze.path.join(WORK, 'surface_diff.json'), JSON.stringify(res, null, 1));
  const b = res.bro;
  const c = res.chromium;
  const be = new Set(b.extensions || []);
  const ce = new Set(c.extensions || []);
  console.log(`== extensions: bro ${be.size}, chromium ${ce.size}`);
  console.log('  only chromium:', Array.from(ce).filter(x => !be.has(x)).sort().join(', '));
  console.log('  only bro:     ', Array.from(be).filter(x => !ce.has(x)).sort().join(', '));
  for (const sect of ['parameters', 'precision', 'attributes', 'formats', 'misc']) {
    console.log('== ' + sect);
    const bs = b[sect] || {};
    const cs = c[sect] || {};
    const keys = Array.from(new Set([...Object.keys(bs), ...Object.keys(cs)])).sort();
    for (const k of keys) {
      const vb = bs[k];
      const vc = cs[k];
      const same = deepEqual(vb, vc);
      if (!same || opts.all) {
        console.log(`  ${same ? '  ' : '!='} ${k.padEnd(44)} bro=${JSON.stringify(vb)}  chromium=${JSON.stringify(vc)}`);
      }
    }
  }
}

function parseCalls(path) {
  const calls = [];
  const strings = new Map();
  let cur = null;
  for (const line of splitLines(Bronze.fs.readText(path))) {
    if (line.startsWith('#### str ')) {
      cur = line.slice(9).trim();
      strings.set(cur, []);
      continue;
    }
    if (cur !== null) strings.get(cur).push(line);
    else calls.push(line);
  }
  const strObj = new Map();
  for (const [k, v] of strings.entries()) strObj.set(k, v.join('\n'));
  return [calls, strObj];
}

function diffLines(a, b) {
  const out = ['--- bro', '+++ chromium'];
  let i = 0, j = 0;
  while (i < a.length || j < b.length) {
    if (i < a.length && j < b.length && a[i] === b[j]) {
      i++; j++;
      continue;
    }
    if (i < a.length) out.push('-' + a[i++]);
    if (j < b.length) out.push('+' + b[j++]);
  }
  return out;
}

function countBy(items, keyFn) {
  const m = new Map();
  for (const x of items) {
    const k = keyFn(x);
    if (k !== null) m.set(k, (m.get(k) || 0) + 1);
  }
  return m;
}

function cmdRecord(argv) {
  const { opts } = parseArgs(argv, {
    scenes: { default: '' },
    n: { type: 'int', default: 0 },
  });
  const scenes = opts.scenes ? opts.scenes.split(',') : SCENES;
  for (const scene of scenes) {
    const n = opts.n || SMALL[scene];
    const cfg = { mode: 'record', n, rec: 3 };
    const got = {};
    for (const browser of ['bro', 'chromium']) {
      const r = windowed(browser, `${scene}.html`, cfg, `rec_${browser}_${scene}`);
      const p = Bronze.path.join(r.out, 'calls.txt');
      if (!Bronze.fs.exists(p)) {
        console.log(scene, browser, 'no calls.txt; status', r.status);
        console.log(r.log.filter(l => l.includes('RW') || l.includes('rror')).join('\n').slice(-1500));
        break;
      }
      got[browser] = parseCalls(p);
    }
    if (!got.bro || !got.chromium) continue;
    const [bc, bs] = got.bro;
    const [cc, cs] = got.chromium;
    const diff = diffLines(bc, cc);
    const diffPath = Bronze.path.join(WORK, `rec_${scene}.diff`);
    Bronze.fs.writeText(diffPath, diff.join('\n') + '\n');
    const nd = diff.filter(l => (l.startsWith('+') || l.startsWith('-')) && !l.startsWith('+++') && !l.startsWith('---')).length;
    console.log(`== ${scene} N=${n}: bro ${bc.length} calls, chromium ${cc.length} calls, ${nd} differing lines -> ${diffPath}`);
    const fnOf = l => l.includes('\t') ? l.split('\t')[1].split('(')[0] : null;
    const frameOf = l => l.includes('\t') ? l.split('\t')[0] : null;
    const pb = countBy(bc, frameOf);
    const pc = countBy(cc, frameOf);
    const frames = Array.from(new Set([...pb.keys(), ...pc.keys()])).sort((x, y) => parseInt(x, 10) - parseInt(y, 10));
    console.log('   per frame (bro/chromium): ' + frames.map(f =>
      `f${f}: ${pb.get(f) || 0}/${pc.get(f) || 0}${(pb.get(f) || 0) === (pc.get(f) || 0) ? '' : ' !='}`).join('  '));
    for (const f of frames) {
      const fb = countBy(bc.filter(l => l.startsWith(f + '\t')), fnOf);
      const fc = countBy(cc.filter(l => l.startsWith(f + '\t')), fnOf);
      for (const fn of Array.from(new Set([...fb.keys(), ...fc.keys()])).sort()) {
        if ((fb.get(fn) || 0) !== (fc.get(fn) || 0)) {
          console.log(`     f${f} ${fn.padEnd(38)} bro ${String(fb.get(fn) || 0).padStart(5)}  chromium ${String(fc.get(fn) || 0).padStart(5)}`);
        }
      }
    }
    const kb = countBy(bc, fnOf);
    const kc = countBy(cc, fnOf);
    for (const fn of Array.from(new Set([...kb.keys(), ...kc.keys()])).sort()) {
      if ((kb.get(fn) || 0) !== (kc.get(fn) || 0)) {
        console.log(`   ${fn.padEnd(40)} bro ${String(kb.get(fn) || 0).padStart(5)}  chromium ${String(kc.get(fn) || 0).padStart(5)}`);
      }
    }
    for (const h of Array.from(new Set([...bs.keys(), ...cs.keys()])).sort()) {
      if (bs.has(h) !== cs.has(h)) console.log('   string only in', bs.has(h) ? 'bro' : 'chromium', h);
    }
  }
}

function cmdShot(argv) {
  const { opts } = parseArgs(argv, {
    scene: { default: '' },
    n: { type: 'int', default: 0 },
    frame: { type: 'int', default: 30 },
    tol: { type: 'int', default: 8 },
  });
  if (!opts.scene) {
    console.error('usage: rw shot --scene S [--n N] [--frame 30] [--tol 8]');
    Bronze.exit(2);
  }
  const n = opts.n || SMALL[opts.scene];
  const cfg = { mode: 'shot', n, shot: opts.frame };
  const outs = {};
  for (const browser of ['bro', 'chromium']) {
    const r = windowed(browser, `${opts.scene}.html`, cfg, `shot_${browser}_${opts.scene}`);
    const p = Bronze.path.join(r.out, 'shot.rgba');
    if (!Bronze.fs.exists(p)) {
      console.log(browser, 'no shot; status', r.status);
      console.log(r.log.filter(l => l.includes('RW') || l.includes('rror')).join('\n').slice(-1500));
      return;
    }
    outs[browser] = p;
  }
  compareShots(outs.bro, outs.chromium, {
    size: '1280x720',
    tol: opts.tol,
    out: Bronze.path.join(WORK, `shot_${opts.scene}`),
  });
}

// ---- Entry ------------------------------------------------------------------

const [cmd, ...rest] = Bronze.args;
if (!cmd || cmd === '-h' || cmd === '--help') {
  console.log('usage: rw <l1|surface|record|shot|render|time|perf|sweep|report|js|compare> [args...]');
  Bronze.exit(cmd ? 0 : 2);
}

Bronze.fs.mkdir(WORK);
if (cmd !== 'l1') {
  for (const entry of Bronze.fs.listDir(WORK)) {
    if (entry.startsWith('chprof_')) Bronze.fs.rm(Bronze.path.join(WORK, entry));
  }
}

const dispatch = {
  l1: cmdL1,
  surface: cmdSurface,
  record: cmdRecord,
  shot: cmdShot,
  render: cmdRender,
  time: cmdTime,
  perf: cmdPerf,
  sweep: cmdSweep,
  report: cmdReport,
  js: cmdJs,
  compare: runCompareCli,
};

if (!dispatch[cmd]) {
  console.error(`rw: unknown subcommand '${cmd}'`);
  Bronze.exit(2);
}
dispatch[cmd](rest);
