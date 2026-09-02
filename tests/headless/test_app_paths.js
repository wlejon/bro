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

// ── userDataDir ────────────────────────────────────────────────────────────
//
// Where the app WRITES. appDir is where it is installed — a repo checkout, an
// install directory, a zip mount — and none of those are a place for a save.

assert(typeof bro.userDataDir === 'string', 'bro.userDataDir is a string');
assert(bro.userDataDir.length > 0, 'bro.userDataDir is set when running an app');
assert(path.isAbsolute(bro.userDataDir), 'bro.userDataDir is absolute: ' + bro.userDataDir);
assert(fs.existsSync(bro.userDataDir), 'reading bro.userDataDir creates it: ' + bro.userDataDir);
assert(fs.statSync(bro.userDataDir).isDirectory(), 'bro.userDataDir is a directory');
assert(path.basename(bro.userDataDir) === path.basename(bro.appDir),
       'the app is identified by its folder name (' + bro.userDataDir + ' vs ' + bro.appDir + ')');
assert(path.basename(path.dirname(bro.userDataDir)) === 'apps',
       'apps live under <user data>/bro/apps/');
assert(path.resolve(bro.userDataDir) !== path.resolve(bro.appDir),
       'userDataDir is not the app directory');
assert(bro.userDataDir === bro.userDataDir, 'stable across reads');

// It is writable, which is the point.
const probeFile = path.join(bro.userDataDir, '.paths-probe');
fs.writeFileSync(probeFile, 'ok');
assert(fs.readFileSync(probeFile, 'utf-8') === 'ok', 'a file written under userDataDir reads back');
fs.unlinkSync(probeFile);

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
