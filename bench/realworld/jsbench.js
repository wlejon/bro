// `rw js`: the scenes' JavaScript alone, node against a bronze-built executable.

import {
  ROOT, SCENES, WORK, parseArgs, roundN, rwLines, sha256Hex, splitLines, utf8Bytes,
} from './rw_core.js';

const JS_WORK = Bronze.path.join(WORK, 'js');
const DEFAULT_BRONZE = Bronze.path.resolve(ROOT, '..', '..', '..', 'bronze', 'build', 'src', 'cli', 'bronze');
const CORE = '4';
const COUNTERS = ['instructions:u', 'cycles:u'];
const FRONTEND_COUNTERS = [
  'ic_tag_hit_miss.instruction_cache_miss:u',
  'op_cache_hit_miss.op_cache_miss:u',
  'de_src_op_disp.x86_decoder:u',
];
const DEFAULT_N = {
  three_meshes: 2565,
  three_skinned: 410,
  three_instanced: 42992,
  three_lights: 64,
  pixi_sprites: 10000,
  pixi_graphics: 2000,
  pixi_text: 400,
};
const DEFAULT_FRAMES = { pixi_graphics: [30, 100] };

function sourcesDigest(scene) {
  const files = [Bronze.path.join(ROOT, 'scenes', `${scene}.js`)];
  for (const f of Bronze.fs.listDir(Bronze.path.join(ROOT, 'lib'))) {
    if (f.endsWith('.js')) files.push(Bronze.path.join(ROOT, 'lib', f));
  }
  for (const f of Bronze.fs.listDir(Bronze.path.join(ROOT, 'vendor'))) {
    const p = Bronze.path.join(ROOT, 'vendor', f);
    if (Bronze.fs.isFile(p)) files.push(p);
  }
  files.sort();
  const chunks = [];
  for (const p of files) {
    chunks.push(utf8Bytes(Bronze.path.relative(ROOT, p)));
    chunks.push(Bronze.fs.readBytes(p));
  }
  return sha256Hex(chunks);
}

function buildKey(scene, cfg, bronze, flags, stage, gen = '') {
  const st = Bronze.fs.stat(bronze) || { mtimeMs: 0, size: 0 };
  const payload = JSON.stringify([
    bronze, st.mtimeMs, st.size, flags, cfg, stage,
    sourcesDigest(scene), sha256Hex([utf8Bytes(gen)]),
  ]);
  return sha256Hex([utf8Bytes(payload)]).slice(0, 16);
}

const genCache = new Map();

function generatedFunctions(scene, cfg) {
  if (!scene.startsWith('pixi')) return '';
  const key = scene + '\0' + JSON.stringify(cfg);
  if (genCache.has(key)) return genCache.get(key);
  const sceneWork = Bronze.path.join(JS_WORK, scene);
  Bronze.fs.mkdir(sceneWork);
  const cap = Bronze.path.join(sceneWork, 'capture.json');
  Bronze.fs.unlink(cap);
  const runCfg = Object.assign({}, cfg, { mark: 0, capture: cap });
  const args = Object.entries(runCfg).map(([k, v]) => `${k}=${v}`);
  const res = Bronze.exec(['node', Bronze.path.join(ROOT, 'scenes', `${scene}.js`), ...args], { timeoutMs: 300000 });
  if (res.status !== 0 || !Bronze.fs.exists(cap)) {
    throw new Error(`node capture run of ${scene} failed:\n` + (res.stdout + res.stderr).slice(-2000));
  }
  const out = [
    '// The bodies pixi generated with `new Function` in a node run (rw js), as',
    '// ordinary functions: a bronze-built executable has no eval.',
    'const T = new Map();',
  ];
  for (const [k, fargs] of JSON.parse(Bronze.fs.readText(cap))) {
    const params = fargs.slice(0, -1);
    const body = fargs.length > 0 ? fargs[fargs.length - 1] : '';
    out.push(`T.set(${JSON.stringify(k)}, function (${params.join(', ')}) {\n${body}\n});`);
  }
  out.push(
    'const Real = globalThis.Function;',
    'function BenchFunction(...args) {',
    '    const k = JSON.stringify(args.map(String));',
    '    const f = T.get(k);',
    '    if (f !== undefined) return f;',
    "    throw new Error('mode=bench: new Function(' + k.slice(0, 300) + ') was not seen in the node capture run');",
    '}',
    'BenchFunction.prototype = Real.prototype;',
    'globalThis.Function = BenchFunction;',
    ''
  );
  const code = out.join('\n');
  genCache.set(key, code);
  return code;
}

function bronzeBuild(scene, cfg, bronze, flags, stage = 'plain', extra = []) {
  const gen = generatedFunctions(scene, cfg);
  const key = buildKey(scene, cfg, bronze, [flags, extra], stage, gen);
  const d = Bronze.path.join(JS_WORK, scene, key);
  const exe = Bronze.path.join(d, 'bench');
  if (Bronze.fs.exists(exe) && Bronze.fs.exists(Bronze.path.join(d, 'bench.so'))) {
    return [exe, null];
  }
  Bronze.fs.rm(d);
  Bronze.fs.mkdir(d);
  Bronze.fs.writeText(Bronze.path.join(d, 'cfg.js'), 'globalThis.BENCH_CONFIG = ' + JSON.stringify(cfg) + ';\n');
  const rel = Bronze.path.relative(d, Bronze.path.join(ROOT, 'scenes', `${scene}.js`));
  let main = `import './cfg.js';\nimport '${rel}';\n`;
  if (gen) {
    Bronze.fs.writeText(Bronze.path.join(d, 'gen.js'), gen);
    main = "import './gen.js';\n" + main;
  }
  Bronze.fs.writeText(Bronze.path.join(d, 'main.js'), main);
  const flagList = flags.trim() ? flags.trim().split(/\s+/) : [];
  const cmd = [bronze, 'build', Bronze.path.join(d, 'main.js'), '-o', exe, ...flagList, ...extra];
  const t0 = performance.now();
  const res = Bronze.exec(cmd, { cwd: d, timeoutMs: 600000 });
  const secs = roundN((performance.now() - t0) / 1000.0, 1);
  Bronze.fs.writeText(Bronze.path.join(d, 'build.log'), res.stdout + res.stderr);
  if (res.status !== 0 || !Bronze.fs.exists(exe)) {
    Bronze.fs.unlink(exe);
    throw new Error(`bronze build ${scene} failed (${Bronze.path.join(d, 'build.log')}):\n` + (res.stdout + res.stderr).slice(-3000));
  }
  return [exe, secs];
}

function harvestedBuild(scene, cfg, bronze, flags) {
  const gen = generatedFunctions(scene, cfg);
  const tfb = Bronze.path.join(JS_WORK, scene, buildKey(scene, cfg, bronze, flags, 'harvest', gen) + '.tfb');
  const fedFlags = (flags + ' --feedback-in ' + tfb).trim();
  const fed = Bronze.path.join(JS_WORK, scene, buildKey(scene, cfg, bronze, [fedFlags, []], 'fed', gen), 'bench');
  if (Bronze.fs.exists(fed) && Bronze.fs.exists(tfb)) return [fed, null];
  const [exe1, secs1] = bronzeBuild(scene, cfg, bronze, flags, 'harvest', ['--harvest-out', tfb]);
  Bronze.fs.unlink(tfb);
  const res = Bronze.exec([exe1], { timeoutMs: 600000 });
  if (res.status !== 0 || !Bronze.fs.exists(tfb)) {
    throw new Error(`harvest run of ${scene} wrote no ${tfb}:\n` + (res.stdout + res.stderr).slice(-2000));
  }
  const [exe2, secs2] = bronzeBuild(scene, cfg, bronze, fedFlags, 'fed');
  return [exe2, roundN((secs1 || 0) + (secs2 || 0), 1)];
}

function countedRun(cmd, perfExtra = null, events = null) {
  const stat = Bronze.path.join(WORK, `rwjs_stat_${Date.now()}_${Math.floor(Math.random() * 1e6)}.txt`);
  const perfPrefix = perfExtra || ['perf', 'stat', '-x,', '-e', (events || COUNTERS).join(','), '-o', stat];
  const child = ['taskset', '-c', CORE, ...cmd];
  const r = Bronze.execPerfMarked(perfPrefix, child, { markOn: 'RW mark on', markOff: 'RW mark off' });
  const counts = {};
  if (Bronze.fs.exists(stat)) {
    for (const line of splitLines(Bronze.fs.readText(stat))) {
      const parts = line.split(',');
      if (parts.length > 2 && /^\d+$/.test(parts[0].trim())) {
        counts[parts[2]] = parseInt(parts[0].trim(), 10);
      }
    }
    Bronze.fs.unlink(stat);
  }
  if (r.status !== 0 || r.marks < 2) {
    throw new Error(`run failed (rc=${r.status}, marks=${r.marks}): ${cmd.join(' ')}\n` +
      r.stdout.slice(-1500) + r.stderr.slice(-1500));
  }
  return [r.stdout, counts];
}

function benchLine(text) {
  const lines = rwLines(text);
  for (const [tag, obj] of lines) {
    if (tag === 'bench') return obj;
  }
  const errs = lines.filter(([t]) => t === 'error').map(([, o]) => o);
  throw new Error('no RW bench line' + (errs.length ? ': ' + JSON.stringify(errs[0]).slice(0, 800) : ''));
}

function measure(cmd, frames, reps = 2, events = null) {
  let best = null;
  for (let i = 0; i < reps; i++) {
    const [text, counts] = countedRun(cmd, null, events);
    const b = benchLine(text);
    const per = {};
    for (const [k, v] of Object.entries(counts)) per[k] = v / frames;
    if (!best || b.ms_per_frame < best[0].ms_per_frame) {
      best = [b, per['instructions:u'] || 0, per['cycles:u'] || 0, per];
    }
  }
  return best;
}

function sceneCfg(scene, opts) {
  const [defW, defF] = DEFAULT_FRAMES[scene] || [60, 300];
  return {
    mode: 'bench',
    n: opts.n || DEFAULT_N[scene],
    warm: opts.warm || defW,
    frames: opts.frames || defF,
    mark: 1,
  };
}

function fmtN(x) {
  if (x >= 1e6) return (x / 1e6).toFixed(2) + 'M';
  if (x >= 1e3) return (x / 1e3).toFixed(1) + 'k';
  return x.toFixed(0);
}

function profileScene(scene, opts, bronze) {
  const cfg = sceneCfg(scene, opts);
  const [exe] = opts.harvest
    ? harvestedBuild(scene, cfg, bronze, opts.flags)
    : bronzeBuild(scene, cfg, bronze, opts.flags);
  const data = Bronze.path.join(JS_WORK, scene, 'perf.data');
  const perf = ['perf', 'record', '-q', '-e', 'cycles:u', '-F', '4999', '-o', data];
  const [text] = countedRun([exe], perf);
  const b = benchLine(text);
  console.log(`${scene} n=${cfg.n}: bronze ${b.ms_per_frame.toFixed(3)} ms/frame (${exe})`);
  const res = Bronze.exec(['perf', 'report', '-i', data, '--stdio', '--no-children', '--sort', 'dso,symbol', '-q', '--percent-limit', '0.2']);
  const lines = splitLines(res.stdout).filter(l => l.trim());
  for (const l of lines.slice(0, 25)) console.log(l.slice(0, 160));
}

function instructionProfile(cmd, data, frames) {
  Bronze.exec(['perf', 'record', '-q', '-e', 'ibs_op/cnt_ctl=1/', '-c', '200003', '-o', data, '--', 'taskset', '-c', CORE, ...cmd]);
  const res = Bronze.exec(['perf', 'report', '-i', data, '--stdio', '--no-children', '--sort', 'symbol', '-F', 'period,symbol', '-q']);
  const stat = data + '.stat';
  Bronze.exec(['perf', 'stat', '-x,', '-e', 'instructions:u', '-o', stat, '--', 'taskset', '-c', CORE, ...cmd]);
  const instrLine = splitLines(Bronze.fs.readText(stat)).find(l => l.includes('instructions'));
  const run = instrLine ? parseInt(instrLine.split(',')[0], 10) : 0;
  Bronze.fs.unlink(stat);
  const rows = [];
  for (const l of splitLines(res.stdout)) {
    const m = /^\s*(\d+)\s+\S+\s+(.*)$/.exec(l);
    if (m) rows.push([parseInt(m[1], 10), m[2]]);
  }
  const sampled = rows.reduce((a, r) => a + r[0], 0) || 1;
  const out = new Map();
  for (const [period, rawSym] of rows) {
    const n = (period / sampled) * run / frames;
    let sym = rawSym.split('  ')[0].trim();
    if (/^(JS:|LazyCompile:|Function:|Script:)/.test(sym)) {
      sym = sym.slice(sym.indexOf(':') + 1).replace(/^[a-z]?[*~^+']+/, '').split(' ')[0] || '(anonymous)';
    }
    let name = sym.split('(')[0].trim();
    if (/^0x[0-9a-f]+$/.test(name)) name = '(unresolved)';
    name = name.replace(/(\$\w+|__generic|__twin)+$/g, '');
    const tail = name.split('.').pop();
    if (tail === 'x' || tail === 'y' || name.startsWith('get ') || name.startsWith('set ')) {
      name = name.split(/\s+/).slice(-2).join(' ');
    } else {
      name = tail;
    }
    out.set(name, (out.get(name) || 0) + n);
  }
  return out;
}

function versusScene(scene, opts, bronze) {
  const cfg = sceneCfg(scene, opts);
  const [exe] = opts.harvest
    ? harvestedBuild(scene, cfg, bronze, opts.flags)
    : bronzeBuild(scene, cfg, bronze, opts.flags);
  const work = Bronze.path.join(JS_WORK, scene);
  const cfgArgs = Object.entries(cfg).map(([k, v]) => `${k}=${v}`);
  const nodeCmd = ['node', '--perf-basic-prof', Bronze.path.join(ROOT, 'scenes', `${scene}.js`), ...cfgArgs];
  const steps = cfg.frames + (cfg.warm || 0);
  const b = instructionProfile([exe], Bronze.path.join(work, 'perf.instr.data'), steps);
  const n = instructionProfile(nodeCmd, Bronze.path.join(work, 'perf.node.data'), steps);
  const plainNode = ['node', Bronze.path.join(ROOT, 'scenes', `${scene}.js`), ...cfgArgs];
  const bt = measure([exe], cfg.frames, 1)[1];
  const nt = measure(plainNode, cfg.frames, 1)[1];
  try {
    for (const f of Bronze.fs.listDir('/tmp')) {
      if (f.startsWith('perf-') && f.endsWith('.map')) Bronze.fs.unlink('/tmp/' + f);
    }
  } catch (_) {}
  console.log(`${scene} n=${cfg.n}: instructions a frame, bronze ${fmtN(bt)} node ${fmtN(nt)}`);
  console.log(`${'function'.padEnd(44)} ${'bronze'.padStart(9)} ${'node'.padStart(9)} ${'gap'.padStart(9)}`);
  const names = Array.from(new Set([...b.keys(), ...n.keys()])).sort((x, y) => (n.get(x) || 0) - (b.get(x) || 0) - ((n.get(y) || 0) - (b.get(y) || 0)));
  const fmtRow = k => `${k.slice(0, 44).padEnd(44)} ${fmtN(b.get(k) || 0).padStart(9)} ${fmtN(n.get(k) || 0).padStart(9)} ${fmtN((b.get(k) || 0) - (n.get(k) || 0)).padStart(9)}`;
  for (const k of names.slice(0, opts.top)) console.log(fmtRow(k));
  const rest = names.slice(opts.top);
  if (rest.length > 0) {
    console.log('...');
    for (const k of rest.slice(-Math.max(Math.floor(opts.top / 2), 1))) console.log(fmtRow(k));
  }
}

export function cmdJs(argv) {
  const { opts, pos } = parseArgs(argv, {
    n: { type: 'int', default: 0 },
    warm: { type: 'int', default: 0 },
    frames: { type: 'int', default: 0 },
    bronze: { default: DEFAULT_BRONZE },
    flags: { default: '' },
    harvest: { bool: true, default: false },
    profile: { bool: true, default: false },
    versus: { bool: true, default: false },
    top: { type: 'int', default: 30 },
    frontend: { bool: true, default: false },
    json: { default: '' },
  });
  if (pos.length === 0) {
    console.error('usage: rw js <scene>|all [--n N] [--warm W] [--frames F] [--bronze PATH] [--harvest] [--profile] [--versus]');
    Bronze.exit(2);
  }
  const bronze = Bronze.path.resolve(opts.bronze);
  const scenes = pos[0] === 'all' ? SCENES : pos[0].split(',');
  if (opts.profile) {
    for (const s of scenes) profileScene(s, opts, bronze);
    return;
  }
  if (opts.versus) {
    for (const s of scenes) versusScene(s, opts, bronze);
    return;
  }

  const rows = [];
  for (const scene of scenes) {
    const cfg = sceneCfg(scene, opts);
    try {
      const [exe, secs] = opts.harvest
        ? harvestedBuild(scene, cfg, bronze, opts.flags)
        : bronzeBuild(scene, cfg, bronze, opts.flags);
      const nodeCmd = ['node', Bronze.path.join(ROOT, 'scenes', `${scene}.js`), ...Object.entries(cfg).map(([k, v]) => `${k}=${v}`)];
      const [nb, ni, nc] = measure(nodeCmd, cfg.frames);
      const events = opts.frontend ? COUNTERS.concat(FRONTEND_COUNTERS) : null;
      const [bb, bi, bc, bper] = measure([exe], cfg.frames, 2, events);
      const ok = nb.checksum === bb.checksum;
      const row = {
        scene, n: cfg.n, frames: cfg.frames,
        node_ms: nb.ms_per_frame, bronze_ms: bb.ms_per_frame,
        node_instr: ni, bronze_instr: bi, node_cycles: nc, bronze_cycles: bc,
        draws: bb.draw_calls_per_frame, gl_calls: bb.gl_calls_per_frame,
        checksum_node: nb.checksum, checksum_bronze: bb.checksum, checksum_ok: ok,
        build_s: secs, exe, harvest: !!opts.harvest,
      };
      if (opts.frontend) {
        row.bronze_l1i_miss = bper[FRONTEND_COUNTERS[0]] || 0;
        row.bronze_opcache_miss = bper[FRONTEND_COUNTERS[1]] || 0;
        row.bronze_decoder_ops = bper[FRONTEND_COUNTERS[2]] || 0;
      }
      rows.push(row);
      if (!ok) {
        console.log(`!!!! ${scene}: CHECKSUM MISMATCH node ${nb.checksum} bronze ${bb.checksum} (node last ${nb.last}, bronze last ${bb.last})`);
      }
      console.log(`   ${scene}: node ${nb.ms_per_frame.toFixed(3)} ms, bronze ${bb.ms_per_frame.toFixed(3)} ms` +
        (secs !== null ? `, built in ${secs} s` : ', cached build'));
    } catch (e) {
      console.log(`${scene}: ERROR ${String(e)}`);
      rows.push({ scene, error: String(e).slice(0, 2000) });
    }
  }

  console.log('');
  const title = 'bronze' + (opts.harvest ? ' (harvest)' : '');
  const titleW = Math.max(10, title.length + 3);
  const feHead = opts.frontend ? ` ${'cyc/f'.padStart(8)} ${'L1I miss/f'.padStart(10)} ${'op$ miss/f'.padStart(10)} ${'dec ops/f'.padStart(9)}` : '';
  console.log(
    `${'scene'.padEnd(16)} ${'n'.padStart(6)} ${'node ms'.padStart(8)} ${(title + ' ms').padStart(titleW)} ${'ratio'.padStart(6)}` +
    ` ${'node ins/f'.padStart(11)} ${'bronze ins/f'.padStart(12)} ${'ratio'.padStart(6)}${feHead} ${'draws'.padStart(6)} ${'gl calls'.padStart(8)}  check  build`
  );
  for (const r of rows) {
    if (r.error) {
      console.log(`${r.scene.padEnd(16)} ERROR: ${splitLines(r.error)[0].slice(0, 100)}`);
      continue;
    }
    const ratio = r.node_ms ? r.bronze_ms / r.node_ms : 0;
    const iratio = r.node_instr ? r.bronze_instr / r.node_instr : 0;
    const build = r.build_s === null ? 'cached' : `${r.build_s} s`;
    const fe = opts.frontend
      ? ` ${fmtN(r.bronze_cycles).padStart(8)} ${fmtN(r.bronze_l1i_miss).padStart(10)} ${fmtN(r.bronze_opcache_miss).padStart(10)} ${fmtN(r.bronze_decoder_ops).padStart(9)}`
      : '';
    console.log(
      `${r.scene.padEnd(16)} ${String(r.n).padStart(6)} ${r.node_ms.toFixed(3).padStart(8)} ${r.bronze_ms.toFixed(3).padStart(titleW)} ${ratio.toFixed(2).padStart(6)}` +
      ` ${fmtN(r.node_instr).padStart(11)} ${fmtN(r.bronze_instr).padStart(12)} ${iratio.toFixed(2).padStart(6)}${fe}` +
      ` ${r.draws.toFixed(0).padStart(6)} ${r.gl_calls.toFixed(0).padStart(8)}  ${r.checksum_ok ? 'ok' : 'MISMATCH'}  ${build}`
    );
  }
  if (rows.some(r => r.checksum_ok === false)) {
    console.log('\n!!!! CHECKSUM MISMATCH: node and bronze did not do the same work (see above)');
  }
  if (opts.json) Bronze.fs.writeText(opts.json, JSON.stringify(rows, null, 1));
}
