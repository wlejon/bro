// bro.models — on-demand model-weight acquisition.
//
// Apps declare the files they need (typically in their bro.json "models"
// array) and call bro.models.ensure(specs, { onProgress }) at boot; ensure()
// downloads any not already present into a shared user-data cache
// (<userDataDir>/bro/models, override via BRO_MODELS_DIR) straight from the
// artifacts' upstream Hugging Face homes, then returns { id: resolved-path }
// to feed the existing loaders (bro.lm.loadQwen, bro.stt.loadWhisper,
// bro.wake.listen, ...). resolve(spec) returns a path without downloading,
// preferring an existing dev sibling (spec.dev) so a source checkout never
// re-pulls the large weights it already has.
//
// Pure JS: orchestrates brokit's fetch (streaming, redirect-following), fs,
// and crypto. Evaluated onto the `bro` global at context init.
(function () {
  'use strict';
  var bro = (globalThis.bro = globalThis.bro || {});
  if (bro.models) return;  // idempotent per context

  function env(k) {
    try { var p = globalThis.process; return (p && p.env && p.env[k]) || ''; }
    catch (_) { return ''; }
  }

  // Mirrors system/projects/app.js userDataDir(): the per-OS app-data root.
  function userDataDir() {
    var os = require('os'), path = require('path');
    var home = os.homedir(), plat = os.platform();
    if (plat === 'win32')
      return path.join(env('APPDATA') || path.join(home, 'AppData', 'Roaming'), 'bro');
    if (plat === 'darwin')
      return path.join(home, 'Library', 'Application Support', 'bro');
    return path.join(env('XDG_DATA_HOME') || path.join(home, '.local', 'share'), 'bro');
  }

  function cacheDir() {
    return env('BRO_MODELS_DIR') || require('path').join(userDataDir(), 'models');
  }

  // Per-file cache path: <cacheDir>/<repo>/<file>, with a "datasets/" prefix for
  // dataset repos so it mirrors the HF URL namespace and a model and dataset of
  // the same name can't collide.
  function cachePathFor(spec) {
    var path = require('path');
    var sub = spec.kind === 'dataset' ? path.join('datasets', spec.repo) : spec.repo;
    return path.join(cacheDir(), sub, spec.file);
  }

  function urlFor(spec) {
    var base = 'https://huggingface.co/';
    if (spec.kind === 'dataset') base += 'datasets/';
    return base + spec.repo + '/resolve/main/' + spec.file;
  }

  function sizeOf(p) {
    try { return require('fs').statSync(p).size; } catch (_) { return -1; }
  }

  // resolve(spec) -> absolute path, no download. Prefers an existing cache hit,
  // then an existing dev sibling (so a source checkout never re-pulls the large
  // weights it already has), else the cache path (which ensure() will fill).
  function resolve(spec) {
    var fs = require('fs');
    var cp = cachePathFor(spec);
    if (fs.existsSync(cp)) return cp;
    if (spec.dev && fs.existsSync(spec.dev)) return spec.dev;
    return cp;
  }

  // A spec counts as cached if resolve() finds a real file. Size is only checked
  // against the cache copy (spec.bytes is the upstream size; a dev sibling may
  // be a converted file of a different size, so it is trusted as-is).
  function cached(spec) {
    var p = resolve(spec);
    if (!require('fs').existsSync(p)) return false;
    if (spec.bytes && p === cachePathFor(spec) && sizeOf(p) !== spec.bytes) return false;
    return true;
  }

  async function sha256Hex(bytes) {
    var digest = await crypto.subtle.digest('SHA-256', bytes);
    var b = new Uint8Array(digest), s = '';
    for (var i = 0; i < b.length; i++) s += b[i].toString(16).padStart(2, '0');
    return s;
  }

  async function downloadOne(spec, onProgress) {
    var fs = require('fs'), path = require('path');
    var dest = cachePathFor(spec);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    var part = dest + '.part';
    try { fs.unlinkSync(part); } catch (_) {}

    var headers = {};
    var tok = env('HF_TOKEN');
    if (tok) headers['Authorization'] = 'Bearer ' + tok;

    var url = urlFor(spec);
    var res = await fetch(url, { headers: headers });
    if (!res.ok) throw new Error('HTTP ' + res.status + ' for ' + url);

    var total = spec.bytes || 0;
    try { total = total || parseInt(res.headers.get('content-length') || '0', 10) || 0; }
    catch (_) {}
    var received = 0;

    if (res.body && typeof res.body.getReader === 'function') {
      // Stream to disk so large weights (multi-GB GGUF) never sit in memory.
      var reader = res.body.getReader();
      while (true) {
        var r = await reader.read();
        if (r.done) break;
        var chunk = r.value instanceof Uint8Array ? r.value : new Uint8Array(r.value);
        fs.appendFileSync(part, chunk);
        received += chunk.byteLength;
        if (onProgress) onProgress({ id: spec.id, received: received, total: total });
      }
    } else {
      var ab = await res.arrayBuffer();
      var all = new Uint8Array(ab);
      fs.writeFileSync(part, all);
      received = all.byteLength;
      if (onProgress) onProgress({ id: spec.id, received: received, total: total || received });
    }

    if (spec.bytes && received !== spec.bytes) {
      try { fs.unlinkSync(part); } catch (_) {}
      throw new Error(spec.id + ': downloaded ' + received + ' bytes, expected ' + spec.bytes);
    }
    if (spec.sha256) {
      var got = await sha256Hex(fs.readFileSync(part));
      if (got.toLowerCase() !== String(spec.sha256).toLowerCase()) {
        try { fs.unlinkSync(part); } catch (_) {}
        throw new Error(spec.id + ': sha256 mismatch');
      }
    }
    fs.renameSync(part, dest);
    return dest;
  }

  // ensure(specs, { onProgress }) -> Promise<{ [id]: path }>.
  // Downloads sequentially: a single HF connection already saturates most
  // links, and serial keeps memory + rate-limit pressure low.
  async function ensure(specs, opts) {
    opts = opts || {};
    if (!Array.isArray(specs))
      throw new Error('bro.models.ensure: specs must be an array');
    var out = {};
    for (var i = 0; i < specs.length; i++) {
      var spec = specs[i];
      if (!spec || !spec.id || !spec.repo || !spec.file)
        throw new Error('bro.models.ensure: each spec needs { id, repo, file }');
      if (cached(spec)) {
        out[spec.id] = resolve(spec);
        if (opts.onProgress) {
          var n = sizeOf(out[spec.id]);
          opts.onProgress({ id: spec.id, received: n, total: n, cached: true });
        }
        continue;
      }
      out[spec.id] = await downloadOne(spec, opts.onProgress);
    }
    return out;
  }

  bro.models = {
    cacheDir: cacheDir,
    resolve: resolve,
    ensure: ensure,
  };
})();
