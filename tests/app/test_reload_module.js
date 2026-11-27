// In-place ES module hot-swap via bro.app.reloadModule(path): reloads one
// edited module without tearing down sibling modules or DOM state, returns the
// updated namespace, and binds subsequent import() calls to the new instance.
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
assert(headless, 'a bro-headless binary');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_reload_mod_'));

try {
    const appDir = path.join(root, 'app');
    fs.mkdirSync(appDir);

    const rulesPath = path.join(appDir, 'rules.js');
    fs.writeFileSync(path.join(appDir, 'state.js'), 'export const state = { ticks: 0 };\nexport function bump() { return ++state.ticks; }\n');
    fs.writeFileSync(rulesPath, 'export const version = 1;\nexport function compute(x) { return x + 10; }\n');
    fs.writeFileSync(path.join(appDir, 'main.js'),
        'import { state, bump } from "./state.js";\n' +
        'import { version, compute } from "./rules.js";\n' +
        'bump();\n' +
        'window.__boot = { ticks: state.ticks, version, val: compute(5) };\n' +
        'window.__getStateTicks = () => state.ticks;\n');
    fs.writeFileSync(path.join(appDir, 'index.html'),
        '<!doctype html><html><body><div id="keep">alive</div><script type="module" src="main.js"></script></body></html>\n');

    const probe = path.join(root, 'probe.js');
    fs.writeFileSync(probe,
        'assert(window.__boot.ticks === 1 && window.__boot.version === 1 && window.__boot.val === 15, "initial boot ok");\n' +
        'const fsMod = require("fs");\n' +
        'fsMod.writeFileSync(' + JSON.stringify(rulesPath) + ', "export const version = 2;\\nexport function compute(x) { return x * 10; }\\n");\n' +
        'const ns = bro.app.reloadModule("/app/rules.js");\n' +
        'assert(ns && ns.version === 2 && ns.compute(5) === 50, "reloadModule returned updated namespace");\n' +
        'const modPath = "/app/rules.js";\n' +
        'const dyn = await import(modPath);\n' +
        'assert(dyn === ns && dyn.version === 2 && dyn.compute(6) === 60, "subsequent import(expr) sees swapped namespace");\n' +
        'const postEval = await eval("import(\\"/app/rules.js\\")");\n' +
        'assert(postEval === ns && postEval.version === 2, "post-swap compiled import() sees swapped namespace");\n' +
        'assert(window.__getStateTicks() === 1, "sibling module state preserved in place");\n' +
        'assert(document.getElementById("keep").textContent === "alive", "DOM preserved in place");\n' +
        'let missingThrew = false;\n' +
        'try { bro.app.reloadModule("/app/does_not_exist.js"); } catch (e) { missingThrew = true; }\n' +
        'assert(missingThrew, "missing module throws cleanly");\n' +
        'console.log("RELOAD_MODULE_INNER_OK");\n');

    const r = cp.spawnSync(headless, [appDir, probe], { encoding: 'utf-8', timeout: 60000 });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(r.status === 0 && out.includes('RELOAD_MODULE_INNER_OK'),
           'test_reload_module inner run passed:\n' + out.slice(-1500));
    console.log('test_reload_module OK');
} finally {
    fs.rmSync(root, { recursive: true, force: true });
}
