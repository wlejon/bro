// bro.appDir / bro.resolvePath — an app naming its own files on the real
// filesystem.
//
// Reading an app's own assets already works through the /app mount, but that
// path is virtual. An app that ships a sidecar binary, or hands a path to an
// external command-line tool, needs something a process can actually open.
// Nothing else supplies it: process.cwd() is wherever the user launched bro
// from, and __dirname exists only inside require()'d modules.

const fs = require('fs');
const path = require('path');

// ── appDir ─────────────────────────────────────────────────────────────────

assert(typeof bro.appDir === 'string', 'bro.appDir is a string');
assert(bro.appDir.length > 0, 'bro.appDir is set when running an app');
assert(fs.existsSync(bro.appDir), 'bro.appDir names a directory that exists: ' + bro.appDir);
assert(fs.existsSync(path.join(bro.appDir, 'index.html')),
       'bro.appDir is the APP directory, not the launch directory (no index.html in ' +
       bro.appDir + ')');

// The launch directory is a different thing, and that difference is the whole
// point. Asserting they differ would depend on where the suite was started
// from, so just assert the useful half: appDir holds the app.
assert(typeof process.cwd() === 'string', 'process.cwd() still works');

// ── resolvePath: the four spellings ────────────────────────────────────────

// Relative — against the app directory, NOT the cwd.
const rel = bro.resolvePath('index.html');
assert(fs.existsSync(rel), 'relative path resolves against the app dir: ' + rel);

// Mount path.
const mounted = bro.resolvePath('/app/index.html');
assert(fs.existsSync(mounted), '/app mount resolves to a real file: ' + mounted);

// A bare mount root resolves to the directory itself — this is how an app
// finds a bundled bin/ or models/ folder.
assert(bro.resolvePath('/app') === bro.appDir,
       "resolvePath('/app') equals appDir (got " + bro.resolvePath('/app') + ')');

// Already-absolute paths pass through untouched, so a caller can hand user
// input straight in without first testing what shape it is.
const abs = bro.resolvePath(bro.appDir);
assert(abs === bro.appDir, 'an absolute path passes through unchanged');

// A path that does not exist yet still resolves — resolution is textual, so it
// works for a file the app is about to create or a binary it may not ship.
const notThere = bro.resolvePath('bin/does-not-exist.bin');
assert(notThere.indexOf('does-not-exist.bin') !== -1, 'missing files still resolve');
assert(notThere.length > 'bin/does-not-exist.bin'.length,
       'a missing file resolves to an absolute path, not the input');
assert(!fs.existsSync(notThere), 'resolving does not create anything');

// ── the resolved path is usable by a real process ──────────────────────────
//
// The reason this binding exists. A mount path is meaningless to a child
// process; the resolved one must not be.

const cp = require('child_process');
const isWin = process.platform === 'win32';
const target = bro.resolvePath('/app/index.html');
const probe = isWin ? { file: 'findstr', args: ['DOCTYPE', target] }
                    : { file: 'grep',    args: ['DOCTYPE', target] };
const res = cp.spawnSync(probe.file, probe.args);
assert(res.status === 0,
       'an external process can open the resolved path (' + target +
       ', status ' + res.status + ')');

console.log('app paths OK — appDir=' + bro.appDir);
