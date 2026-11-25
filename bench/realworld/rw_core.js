// Shared core for bro's real-world three.js / pixi.js harness (`rw.js`, `timing.js`, `jsbench.js`).

function detectRoot() {
  const explicit = Bronze.getenv('RW_ROOT');
  if (explicit) return Bronze.path.resolve(explicit);
  const candidates = [
    Bronze.path.join(Bronze.cwd(), 'bench', 'realworld'),
    Bronze.cwd(),
    Bronze.path.dirname(Bronze.exe),
    Bronze.path.join(Bronze.path.dirname(Bronze.exe), '..', 'bench', 'realworld'),
  ];
  for (const c of candidates) {
    if (Bronze.fs.isDir(Bronze.path.join(c, 'scenes')) && Bronze.fs.isDir(Bronze.path.join(c, 'lib'))) {
      return Bronze.path.resolve(c);
    }
  }
  return Bronze.path.resolve(Bronze.path.join(Bronze.cwd(), 'bench', 'realworld'));
}

export const ROOT = detectRoot();
export const BRO_ROOT = Bronze.path.resolve(ROOT, '..', '..');
export const BUILD = Bronze.path.resolve(Bronze.getenv('RW_BUILD') || Bronze.path.join(BRO_ROOT, 'build-release'));
const EXE_EXT = Bronze.platform === 'windows' ? '.exe' : '';
export const BRO = Bronze.path.join(BUILD, 'bro' + EXE_EXT);
export const BRO_HEADLESS = Bronze.path.join(BUILD, 'bro-headless' + EXE_EXT);
export const WORK = Bronze.path.resolve(Bronze.getenv('RW_WORK') || Bronze.path.join(ROOT, '.work'));

export const AOT_BUILD_S = new Map();

export const SCENES = [
  'three_meshes', 'three_instanced', 'three_skinned', 'three_lights',
  'pixi_sprites', 'pixi_graphics', 'pixi_text',
];

export const SIZES = {
  three_meshes: [100, 10000],
  three_instanced: [1000, 200000],
  three_skinned: [4, 300],
  three_lights: [1, 64],
  pixi_sprites: [1000, 200000],
  pixi_graphics: [100, 10000],
  pixi_text: [20, 2000],
};

export const SMALL = {
  three_meshes: 50,
  three_instanced: 500,
  three_skinned: 4,
  three_lights: 4,
  pixi_sprites: 200,
  pixi_graphics: 40,
  pixi_text: 12,
};

export const TIERS = {
  jit: ['jit', {}],
  tier0: ['jit', { BRO_JIT_TIER: '0' }],
  tier2: ['jit', { BRO_JIT_TIER: '2' }],
  aot: ['aot', {}],
  deopt3: ['jit', { BRASS_DEOPT_STRESS: '3' }],
  gcstress: ['jit', { BRONZE_GC_STRESS: '1' }],
};

export const WAYLAND_ENV = {
  WAYLAND_DISPLAY: Bronze.getenv('WAYLAND_DISPLAY') || 'wayland-0',
  XDG_RUNTIME_DIR: Bronze.getenv('XDG_RUNTIME_DIR') || '/run/user/1000',
};

export function splitLines(text) {
  if (!text) return [];
  const norm = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const lines = norm.split('\n');
  if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
  return lines;
}

export function roundN(x, d) {
  const f = Math.pow(10, d);
  return Math.round(x * f) / f;
}

export function parseArgs(argv, schema) {
  const opts = {};
  const pos = [];
  for (const [k, def] of Object.entries(schema)) opts[k] = def.default;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const eq = a.indexOf('=');
      const flag = eq >= 0 ? a.slice(2, eq) : a.slice(2);
      const spec = schema[flag];
      if (!spec) {
        console.error(`unknown option --${flag}`);
        Bronze.exit(2);
      }
      if (spec.bool) {
        opts[flag] = eq >= 0 ? (a.slice(eq + 1) !== '0' && a.slice(eq + 1) !== 'false') : true;
      } else {
        const raw = eq >= 0 ? a.slice(eq + 1) : argv[++i];
        if (raw === undefined) {
          console.error(`missing value for --${flag}`);
          Bronze.exit(2);
        }
        opts[flag] = spec.type === 'int' ? parseInt(raw, 10)
          : spec.type === 'float' ? parseFloat(raw)
          : raw;
      }
    } else {
      pos.push(a);
    }
  }
  return { opts, pos };
}

// ---- Pure-JS SHA-256 & Base64 decode ----------------------------------------

const SHA_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

export function utf8Bytes(str) {
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let cp = str.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < str.length) {
      const lo = str.charCodeAt(++i);
      cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
    }
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return new Uint8Array(out);
}

export function sha256Hex(chunks) {
  let totalLen = 0;
  for (const c of chunks) totalLen += c.length;
  const paddedLen = Math.ceil((totalLen + 9) / 64) * 64;
  const buf = new Uint8Array(paddedLen);
  let off = 0;
  for (const c of chunks) {
    buf.set(c, off);
    off += c.length;
  }
  buf[totalLen] = 0x80;
  const bitLenHi = Math.floor((totalLen * 8) / 4294967296);
  const bitLenLo = (totalLen * 8) >>> 0;
  const dv = new DataView(buf.buffer);
  dv.setUint32(paddedLen - 8, bitLenHi, false);
  dv.setUint32(paddedLen - 4, bitLenLo, false);

  let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
  let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  const w = new Uint32Array(64);

  for (let p = 0; p < paddedLen; p += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(p + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const s0 = ((w[i - 15] >>> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >>> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >>> 3);
      const s1 = ((w[i - 2] >>> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >>> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ ((~e) & g);
      const t1 = (h + S1 + ch + SHA_K[i] + w[i]) >>> 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      h = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0; h6 = (h6 + g) >>> 0; h7 = (h7 + h) >>> 0;
  }
  return [h0, h1, h2, h3, h4, h5, h6, h7].map(x => x.toString(16).padStart(8, '0')).join('');
}

const B64_LUT = (() => {
  const lut = new Int8Array(128).fill(-1);
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  for (let i = 0; i < chars.length; i++) lut[chars.charCodeAt(i)] = i;
  return lut;
})();

export function base64Decode(str) {
  const clean = str.replace(/\s+/g, '');
  let pad = 0;
  if (clean.endsWith('==')) pad = 2;
  else if (clean.endsWith('=')) pad = 1;
  const out = new Uint8Array((clean.length * 3) / 4 - pad);
  let pos = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const a = B64_LUT[clean.charCodeAt(i)];
    const b = B64_LUT[clean.charCodeAt(i + 1)];
    const c = clean[i + 2] === '=' ? 0 : B64_LUT[clean.charCodeAt(i + 2)];
    const d = clean[i + 3] === '=' ? 0 : B64_LUT[clean.charCodeAt(i + 3)];
    const v = (a << 18) | (b << 12) | (c << 6) | d;
    out[pos++] = (v >>> 16) & 0xff;
    if (pos < out.length) out[pos++] = (v >>> 8) & 0xff;
    if (pos < out.length) out[pos++] = v & 0xff;
  }
  return out;
}

// ---- Bro run dirs & process drivers -----------------------------------------

export function makeBroApp(name, page, cfg) {
  const d = Bronze.path.join(WORK, name);
  Bronze.fs.rm(d);
  Bronze.fs.mkdir(Bronze.path.join(d, 'out'));
  for (const sub of ['lib', 'scenes', 'vendor']) {
    Bronze.fs.symlink(Bronze.path.join(ROOT, sub), Bronze.path.join(d, sub));
  }
  Bronze.fs.symlink(Bronze.path.join(ROOT, 'package.json'), Bronze.path.join(d, 'package.json'));
  const fullCfg = Object.assign({}, cfg, { out: Bronze.path.join(d, 'out') });
  Bronze.fs.writeText(Bronze.path.join(d, 'config.js'), 'globalThis.BENCH_CONFIG = ' + JSON.stringify(fullCfg) + ';\n');
  Bronze.fs.writeText(Bronze.path.join(d, 'index.html'), Bronze.fs.readText(Bronze.path.join(ROOT, page)));
  Bronze.fs.writeText(Bronze.path.join(d, 'bro.json'), JSON.stringify({
    name,
    width: cfg.w || 1280,
    height: cfg.h || 720,
    splash: false,
    watch: false,
    vsync: false,
    maxFps: 0,
    permissions: ['files'],
  }));
  return d;
}

export function headlessAot(d, runArgs, env) {
  const aot = Bronze.path.join(WORK, Bronze.path.basename(d) + '.aot');
  Bronze.fs.rm(aot);
  const buildCmd = [BRO_HEADLESS, d, '--aot-build', aot];
  const runCmd = [BRO_HEADLESS, '--grant', 'declared', '--aot', aot, d, ...runArgs];
  Bronze.exec(buildCmd, { env, timeoutMs: 300000 });
  Bronze.exec(runCmd, { env, timeoutMs: 300000 });
  const res = Bronze.exec(buildCmd, { env, timeoutMs: 300000 });
  if (res.status !== 0) {
    throw new Error('--aot-build failed:\n' + res.stdout.slice(-2000) + res.stderr.slice(-2000));
  }
  return Bronze.exec(runCmd, { env, timeoutMs: 300000 });
}

export function rwLines(text) {
  const out = [];
  for (const line of splitLines(text)) {
    const i = line.indexOf('RW ');
    if (i < 0) continue;
    const body = line.slice(i + 3);
    const sp = body.indexOf(' ');
    const tag = sp >= 0 ? body.slice(0, sp) : body;
    let js = sp >= 0 ? body.slice(sp + 1) : '';
    const srcIdx = js.indexOf('", source: ');
    if (srcIdx >= 0) js = js.slice(0, srcIdx);
    else if (js.endsWith('", source:')) js = js.slice(0, -10);
    try {
      out.push([tag, JSON.parse(js)]);
    } catch (_) {
      out.push([tag, js]);
    }
  }
  return out;
}

const FATAL_MARKERS = ['Uncaught', 'SyntaxError', 'Failed to load module', 'Failed to fetch dynamically'];

function collectBlobs(log, outDir) {
  const parts = new Map();
  for (const line of log) {
    const i = line.indexOf('RWBLOB ');
    if (i < 0) continue;
    const body = line.slice(i + 7).split('", source:')[0].trim().replace(/^"+|"+$/g, '');
    const m = /^(\S+)\s+(\d+)\s+(\d+)\s+(.*)$/.exec(body);
    if (!m) continue;
    const name = m[1];
    const idx = parseInt(m[2], 10);
    const count = parseInt(m[3], 10);
    const b64 = m[4];
    if (!parts.has(name)) parts.set(name, new Array(count).fill(null));
    parts.get(name)[idx] = base64Decode(b64);
  }
  for (const [name, chunks] of parts.entries()) {
    if (chunks.some(c => c === null)) {
      console.log('blob', name, 'incomplete');
      continue;
    }
    let len = 0;
    for (const c of chunks) len += c.length;
    const merged = new Uint8Array(len);
    let off = 0;
    for (const c of chunks) {
      merged.set(c, off);
      off += c.length;
    }
    Bronze.fs.writeBytes(Bronze.path.join(outDir, Bronze.path.basename(name)), merged);
  }
}

export function runChromium(page, cfg, outDir) {
  Bronze.fs.mkdir(outDir);
  const q = Object.entries(cfg).map(([k, v]) => `${k}=${v}`).join('&');
  const url = 'file://' + Bronze.path.join(ROOT, page) + '?' + q;
  const prof = Bronze.path.join(WORK, `chprof_${Date.now()}_${Math.floor(Math.random() * 1e6)}`);
  Bronze.fs.mkdir(prof);
  const w = cfg.w || 1280;
  const h = cfg.h || 720;
  const cmd = [
    'chromium', '--ozone-platform=wayland', `--user-data-dir=${prof}`, '--no-first-run',
    '--no-default-browser-check', '--enable-logging=stderr', '--v=0', '--ignore-gpu-blocklist',
    '--allow-file-access-from-files', '--disable-extensions',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows', '--disable-features=CalculateNativeWinOcclusion',
    '--disable-ipc-flooding-protection', '--disable-hang-monitor',
    '--disable-gpu-vsync', '--disable-frame-rate-limit',
    `--window-size=${w},${h}`, `--app=${url}`,
  ];
  const r = Bronze.exec(cmd, {
    env: WAYLAND_ENV,
    mergeStderr: true,
    stopOnMatch: ['RW_DONE', ...FATAL_MARKERS],
    liveLogPath: Bronze.path.join(WORK, 'live.log'),
    timeoutMs: 600000,
  });
  Bronze.fs.rm(prof);
  const log = splitLines(r.stdout);
  let status = 'eof';
  for (const line of log) {
    if (line.includes('RW_DONE')) {
      const after = line.split('RW_DONE')[1].trim().split(/\s+/)[0] || '';
      status = after.replace(/^[",]+|[",]+$/g, '');
      break;
    }
    if (FATAL_MARKERS.some(p => line.includes(p)) && !line.includes('RW ') && !line.includes('RWBLOB')) {
      status = 'fatal';
      break;
    }
  }
  collectBlobs(log, outDir);
  Bronze.fs.writeText(
    Bronze.path.join(outDir, 'browser.log'),
    log.filter(l => !l.includes('RWBLOB')).join('\n') + '\n'
  );
  return { status, log, out: outDir };
}

export function buildAppExe(d, name, key) {
  const exeDir = Bronze.path.join(WORK, 'exe', name);
  Bronze.fs.mkdir(exeDir);
  const exe = Bronze.path.join(exeDir, 'rwapp' + EXE_EXT);

  function build() {
    const t0 = performance.now();
    const res = Bronze.exec([BRO, '--build-exe', d, '--out', exe], { timeoutMs: 600000 });
    if (res.status !== 0 || !Bronze.fs.exists(exe)) {
      throw new Error('bro --build-exe failed:\n' + res.stdout.slice(-2000) + res.stderr.slice(-2000));
    }
    return [(performance.now() - t0) / 1000.0, res.stdout + res.stderr];
  }

  const grantRes = Bronze.exec([BRO, '--grant-app', d, 'files'], { timeoutMs: 60000 });
  if (grantRes.status !== 0) {
    throw new Error('bro --grant-app failed: ' + grantRes.stderr);
  }
  let [spent, out] = build();
  if (out.includes("waiting for a first run's profile")) {
    const env = Object.assign({ BRO_TEST_VK_VALIDATION: '0' }, WAYLAND_ENV);
    Bronze.exec([exe], { cwd: d, env, timeoutMs: 600000 });
    const [more, out2] = build();
    spent += more;
    if (out2.includes("waiting for a first run's profile")) {
      throw new Error('the harvest run left a unit without its profile:\n' + out2.slice(-2000));
    }
  }
  AOT_BUILD_S.set(key, roundN(spent, 1));
  return exe;
}

export function runBro(page, cfg, name, aot = false, envExtra = null, perfData = '') {
  const d = makeBroApp(name, page, cfg);
  const env = Object.assign({ BRO_TEST_VK_VALIDATION: '0' }, WAYLAND_ENV, envExtra || {});
  let cmd = [BRO, '--grant', 'declared', d];
  if (aot) cmd = [buildAppExe(d, name, Bronze.path.stem(page))];
  if (perfData) {
    env.BRASS_PERF_MAP = '1';
    cmd = ['perf', 'record', '-q', '-k', 'CLOCK_MONOTONIC', '-e', 'cycles:u', '-F', '1999', '-o', perfData, '--', ...cmd];
  }
  Bronze.exec(cmd, { cwd: d, env, timeoutMs: 600000 });
  const log = [];
  let status = 'eof';
  const logf = Bronze.path.join(d, 'bro.log');
  if (Bronze.fs.exists(logf)) {
    for (const line of splitLines(Bronze.fs.readText(logf))) {
      log.push(line);
      if (line.includes('RW_DONE')) {
        status = (line.split('RW_DONE')[1].trim().split(/\s+/)[0]) || 'eof';
      }
    }
  }
  const outDir = Bronze.path.join(d, 'out');
  Bronze.fs.writeText(Bronze.path.join(outDir, 'browser.log'), log.join('\n') + '\n');
  return { status, log, out: outDir };
}

export function windowed(browser, page, cfg, name) {
  if (browser === 'bro') return runBro(page, cfg, name);
  const outDir = Bronze.path.join(WORK, name, 'out');
  Bronze.fs.rm(outDir);
  return runChromium(page, cfg, outDir);
}
