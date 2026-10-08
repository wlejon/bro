// A reload must evaluate the page's ES modules afresh. The realm's module
// registry (bronze runtime/module_registry.h) publishes every module a page
// evaluated so a later unit binds that instance; a reload that kept it made
// the new page's compile treat every previously imported file as external and
// bind the OLD instance — an edited module never loaded, and a renamed export
// read undefined ("undefined is not a constructor" on the live desktop shell
// after a restructure). Engine::performAppReload clears it.
//
// Driven like test_location_reload_toplevel.js: an inner bro-headless boots a
// generated app whose first run edits its own dependency and calls
// location.reload(); the driver's -e expression runs in the second realm.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.join(os.tmpdir(), 'bro-reload-modreg-' + process.pid);
const app = path.join(root, 'app');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(path.join(app, 'js'), { recursive: true });

const depPath = path.join(app, 'js', 'dep.js');
fs.writeFileSync(path.join(app, 'index.html'),
    '<!DOCTYPE html><html><head><title>boot</title></head><body>' +
    '<script type="module" src="js/main.js"></script></body></html>');
fs.writeFileSync(depPath, 'export class OldName { v() { return 1; } }\n');
fs.writeFileSync(path.join(app, 'js', 'main.js'),
    'import * as dep from "./dep.js";\n' +
    'if (!process.env.BRO_MODREG_STAGE) {\n' +
    '  process.env.BRO_MODREG_STAGE = "1";\n' +
    '  document.title = "first:" + (typeof dep.OldName);\n' +
    '  require("fs").writeFileSync(' + JSON.stringify(depPath) + ',\n' +
    '    "export class NewName { v() { return 2; } }\\n");\n' +
    '  location.reload();\n' +
    '} else {\n' +
    '  document.title = (typeof dep.NewName === "function" && typeof dep.OldName === "undefined")\n' +
    '    ? "fresh:" + new dep.NewName().v() : "stale:" + Object.keys(dep).join(",");\n' +
    '}\n');

const env = Object.assign({}, process.env);
delete env.BRO_MODREG_STAGE;
const run = cp.spawnSync(process.execPath, [app, '-e', "console.log('TITLE=' + document.title)"],
                         { encoding: 'utf8', env });
const out = String(run.stdout) + String(run.stderr);
fs.rmSync(root, { recursive: true, force: true });

assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-1500));
const title = /TITLE=(\S+)/.exec(out);
assert(title, 'inner run printed its title:\n' + out.slice(-1500));
assert(title[1] === 'fresh:2',
       'the reloaded page evaluated the edited module afresh, got ' + title[1]);

console.log('reload re-evaluates edited modules OK');
