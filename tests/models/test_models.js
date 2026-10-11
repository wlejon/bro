// bro.models: where model weights live (docs/models-api.js). The search list
// is BRO_MODELS, then the `models.dirs` setting, then the per-user models
// directory, each absolute and listed once; find(name) answers the first
// <dir>/<name> that is a directory, or null.

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const sep = path.delimiter;
const win = process.platform === 'win32';
const norm = (p) => path.resolve(p);
const same = (a, b) => (win ? a.toLowerCase() === b.toLowerCase() : a === b);

assert(typeof bro.models === 'object', 'bro.models exists');
assert(typeof bro.models.find === 'function', 'bro.models.find is a function');

// ---- the default directory is always last ---------------------------------
const base = bro.models.dirs;
assert(Array.isArray(base) && base.length >= 1, 'dirs is a non-empty array');
const userDir = base[base.length - 1];
assert(path.isAbsolute(userDir), `the default dir is absolute (${userDir})`);
assert(path.basename(userDir) === 'models', `the default dir is a "models" folder (${userDir})`);
assert(bro.models.dirs !== bro.models.dirs, 'each read is a fresh array');

// ---- the setting: a path list, in order, each entry once -------------------
const root = path.join(os.tmpdir(), `bro-models-${process.pid}-${Date.now()}`);
fs.mkdirSync(root, { recursive: true });
const A = path.join(root, 'a');
const B = path.join(root, 'b');
fs.mkdirSync(path.join(A, 'm1'), { recursive: true });
fs.mkdirSync(path.join(B, 'm1'), { recursive: true });
fs.mkdirSync(path.join(B, 'm2', 'tiny'), { recursive: true });
fs.writeFileSync(path.join(B, 'weights.bin'), 'not a directory');

// The app layer (not persisted): the user layer would outlive the test.
bro.settings.setDefault('models.dirs', ` ${A}${sep}${sep}${B}${path.sep}${sep}${A} `);
const dirs = bro.models.dirs;
const ia = dirs.findIndex((d) => same(d, norm(A)));
const ib = dirs.findIndex((d) => same(d, norm(B)));
assert(ia >= 0 && ib > ia, `the setting's entries in order: ${JSON.stringify(dirs)}`);
assert(dirs.filter((d) => same(d, norm(A))).length === 1, 'a repeated entry is listed once');
assert(same(dirs[dirs.length - 1], userDir), 'the default dir stays last');
for (const d of dirs) assert(path.isAbsolute(d), `every dir is absolute (${d})`);

// ---- find -------------------------------------------------------------------
assert(same(bro.models.find('m1'), path.join(norm(A), 'm1')), `the first dir holding m1 wins: ${bro.models.find('m1')}`);
assert(same(bro.models.find('m2'), path.join(norm(B), 'm2')), 'a later dir is searched');
assert(same(bro.models.find('m2/tiny'), path.join(norm(B), 'm2', 'tiny')), 'a nested name');
assert(bro.models.find('weights.bin') === null, 'a file is not a model directory');
assert(bro.models.find('missing') === null, 'a name nowhere is null');

const throwsType = (fn, what) => {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err instanceof TypeError, `${what} throws a TypeError (${err})`);
};
throwsType(() => bro.models.find(''), 'an empty name');
throwsType(() => bro.models.find(), 'no name');
throwsType(() => bro.models.find(42), 'a number');
throwsType(() => bro.models.find('../a/m1'), 'a ".." name');
throwsType(() => bro.models.find('m2/../m1'), 'a ".." segment');
throwsType(() => bro.models.find(path.join(A, 'm1')), 'an absolute name');

// The setting follows a change at once.
bro.settings.setDefault('models.dirs', B);
assert(same(bro.models.find('m1'), path.join(norm(B), 'm1')), 'a changed setting applies to the next find');
bro.settings.setDefault('models.dirs', '');

// ---- BRO_MODELS comes first --------------------------------------------------
// A child bro-headless with the variable set (this process's environment is fixed).
const script = path.join(root, 'child.js');
fs.writeFileSync(script, `bro.settings.setDefault('models.dirs', ${JSON.stringify(A)});
console.log('DIRS=' + JSON.stringify(bro.models.dirs));
console.log('FIND=' + bro.models.find('m1'));`);
const child = spawnSync(process.execPath, [bro.appDir || '.', script], {
    env: { ...process.env, BRO_MODELS: `${B}${sep}${A}` },
    encoding: 'utf8',
});
const out = `${child.stdout || ''}${child.stderr || ''}`;
const dm = /DIRS=(\[.*\])/.exec(out);
assert(dm, `the child listed its dirs: ${out.slice(-800)}`);
const cd = JSON.parse(dm[1]);
assert(same(cd[0], norm(B)) && same(cd[1], norm(A)), `BRO_MODELS first, in its order: ${dm[1]}`);
assert(cd.filter((d) => same(d, norm(A))).length === 1, 'the setting repeating an env entry adds nothing');
assert(path.basename(cd[cd.length - 1]) === 'models', 'the default dir last in the child too');
const fm = /FIND=(.*)/.exec(out);
assert(fm && same(fm[1].trim(), path.join(norm(B), 'm1')), `the env's dir wins over the setting's: ${fm && fm[1]}`);

fs.rmSync(root, { recursive: true, force: true });
console.log('test_models PASSED');
