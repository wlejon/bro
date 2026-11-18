// A page's classic scripts as HTML has them (bronze EvalOptions::classicScript,
// ScriptGoal in bronze_host/eval.h): each script's top-level var and function
// declarations are properties of the global object, made as that script
// starts, so a later script's function is not there yet while an earlier one
// runs; let, const and class are shared by the scripts but not properties;
// top-level `this` is window and the code is sloppy. A script that throws
// keeps what it declared and the next one runs. A var naming a host global
// assigns through it (or not, for a read-only one) as Chromium does.
// Inserted scripts, classic workers and the scripts they import are classic
// scripts too; a module worker's var and a control eval's are not properties
// while it runs (bronze copies them once it has run). The
// expected values are Chromium's for classic_scripts_app/index.html (workers
// and control evals aside). A child bro-headless runs the page: its third
// script throws on purpose, which fails the child's run.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'headless', 'classic_scripts_app');
const r = cp.spawnSync(exe, [appDir, path.join(appDir, 'verify.js')], { encoding: 'utf8', timeout: 60000 });
const out = (r.stdout || '') + (r.stderr || '');
const line = out.split('\n').find((l) => l.indexOf('CLASSIC_OUT ') >= 0);
assert(line, 'the page ran and the driver printed its results:\n' + out);
const R = JSON.parse(line.slice(line.indexOf('CLASSIC_OUT ') + 12));
const errors = out.split('\n').filter((l) => l.indexOf('[ERROR]') >= 0);
assert(errors.length === 1 && errors[0].indexOf('classic-scripts-boom') >= 0,
       'the third script\'s throw is the only error:\n' + errors.join('\n'));

const expect = {
    thisIsGlobal: true,
    v1Desc: '1,true,true,false',
    f1Desc: 'function,true,true,false',
    lexicalOnWindow: 'false,false,false',
    f2Early: 'undefined,false',
    bareAfterWin: 5,
    winAfterBare: 7,
    sloppy: true,
    s2: 'number,1,2,3,function,f1',
    f2Here: 'function,function',
    afterThrow: 'set,fb,number,true,undefined',
    redeclare: 'object,true,true,string,42,string,7,5,5,mine,mine',
    redeclareLater: 'object,true,42,5,mine,mine',
    inserted: 'iv,function,iv,if',
    worker: 'w,function,true,i,function',
    moduleWorker: 'undefined,1',
    ctl: 'undefined,number,function',
};
for (const k of Object.keys(expect)) {
    assert(R[k] === expect[k], k + ': expected ' + JSON.stringify(expect[k]) + ', got ' + JSON.stringify(R[k]));
}
console.log('test_classic_scripts OK');
