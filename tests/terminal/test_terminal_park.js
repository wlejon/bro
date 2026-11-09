// A <terminal>'s session outlives a reload when the page parks it in its
// pagehide handler and the next page adopts it: the same child (same pid),
// its shell state, its screen, and from then on its frames and events go to
// the new element. A parked session nobody adopts is ended once the grace
// period after the new page's boot has passed (ElTerminal::kParkGraceFrames
// and kParkGraceMs).
//
// An inner bro-headless boots a generated app. Its first page spawns two
// shells, sets X=41 in one, and parks both in pagehide ('main', 'orphan').
// The second page adopts 'main' (and finds no 'nope'), and leaves 'orphan'
// alone: `echo $((X+1))` typed into the adopted shell prints 42, `exit 7`
// reaches the new element's exit event, and the orphan's pid is gone after
// the grace period while it was still there when the page booted.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (process.platform === 'win32') {
    skipTest('the shell steps are POSIX sh');
} else {
    const root = path.join(os.tmpdir(), 'bro-terminal-park-' + process.pid);
    const app = path.join(root, 'app');
    fs.rmSync(root, { recursive: true, force: true });
    fs.mkdirSync(app, { recursive: true });
    fs.writeFileSync(path.join(app, 'index.html'),
        '<!DOCTYPE html><html><body><script type="module" src="main.js"></script></body></html>');

    // Whether a pid is a live process (a zombie is not).
    const aliveSrc =
        'function alive(pid) {\n' +
        '  try { process.kill(pid, 0); } catch (_) { return false; }\n' +
        '  try { return !/\\) Z /.test(require("fs").readFileSync("/proc/" + pid + "/stat", "utf8")); }\n' +
        '  catch (_) { return true; }\n' +
        '}\n';

    fs.writeFileSync(path.join(app, 'main.js'), aliveSrc +
        'const boot = Number(process.env.TP_BOOT || 0) + 1;\n' +
        'process.env.TP_BOOT = String(boot);\n' +
        'const t = document.createElement("terminal");\n' +
        'document.body.appendChild(t);\n' +
        'window.tpScreen = () => t.screenText();\n' +
        'window.tpFrame = () => t.frameText();\n' +
        'window.tpWrite = (s) => t.write(s);\n' +
        't.addEventListener("exit", (e) => { window.tpExit = e.detail.exitCode; });\n' +
        'if (boot === 1) {\n' +
        '  const orphan = document.createElement("terminal");\n' +
        '  document.body.appendChild(orphan);\n' +
        '  const sh = { command: "/bin/sh", env: { PS1: "$ " } };\n' +
        '  t.spawn(sh);\n' +
        '  orphan.spawn(sh);\n' +
        '  process.env.TP_PID = String(t.pid);\n' +
        '  process.env.TP_ORPHAN = String(orphan.pid);\n' +
        '  t.write("X=41; echo set-$X\\n");\n' +
        '  addEventListener("pagehide", () => {\n' +
        '    t.park("main");\n' +
        '    orphan.park("orphan");\n' +
        '    process.env.TP_PARKED = String(!t.running && t.pid === 0);\n' +
        '  });\n' +
        '} else {\n' +
        '  const missing = t.adopt("nope");\n' +
        '  const adopted = t.adopt("main");\n' +
        '  process.env.TP_ADOPT = [missing, adopted, t.pid === Number(process.env.TP_PID), t.running].join(",");\n' +
        '  process.env.TP_ORPHAN_AT_BOOT = String(alive(Number(process.env.TP_ORPHAN)));\n' +
        // A key still parked is taken: a second park under it throws.
        '  const other = document.createElement("terminal");\n' +
        '  document.body.appendChild(other);\n' +
        '  other.spawn({ command: "/bin/sh" });\n' +
        '  let taken = "no throw";\n' +
        '  try { other.park("orphan"); } catch (e) { taken = e.name; }\n' +
        '  process.env.TP_TAKEN = taken;\n' +
        '  other.kill();\n' +
        '}\n');

    const steps = aliveSrc + [
        'function waitFor(pred) { for (let i = 0; i < 2000 && !pred(); i++) { wallSleep(5); runFrames(1, { stepMs: 16 }); } return pred(); }',
        'runFrames(3, { stepMs: 16 })',
        'console.log("SET " + waitFor(() => /^set-41\\s*$/m.test(window.tpScreen())))',
        'location.reload()',
        'for (let i = 0; i < 40 && process.env.TP_BOOT !== "2"; i++) runFrames(1, { stepMs: 16 })',
        'console.log("PARKED " + process.env.TP_PARKED)',
        'console.log("ADOPT " + process.env.TP_ADOPT)',
        'console.log("TAKEN " + process.env.TP_TAKEN)',
        'console.log("ORPHAN_AT_BOOT " + process.env.TP_ORPHAN_AT_BOOT)',
        'window.tpWrite("echo $((X+1))\\n")',
        'console.log("ECHO " + waitFor(() => /^42\\s*$/m.test(window.tpFrame())))',
        // Past the grace period (2 frames and 2 s of the engine's clock).
        'for (let i = 0; i < 40; i++) runFrames(1, { stepMs: 100 })',
        'console.log("ORPHAN_AFTER " + !waitFor(() => !alive(Number(process.env.TP_ORPHAN))))',
        'console.log("MAIN_AFTER " + alive(Number(process.env.TP_PID)))',
        'window.tpWrite("exit 7\\n")',
        'console.log("EXIT " + (waitFor(() => window.tpExit !== undefined) && window.tpExit))',
    ].join(';\n');

    const env = Object.assign({}, process.env);
    for (const k of ['TP_BOOT', 'TP_PID', 'TP_ORPHAN', 'TP_PARKED', 'TP_ADOPT', 'TP_TAKEN', 'TP_ORPHAN_AT_BOOT']) delete env[k];
    // The inner app starts shells: its run grants it that.
    const run = cp.spawnSync(process.execPath, ['--grant', 'spawn', app, '-e', steps], { encoding: 'utf8', env });
    const out = String(run.stdout) + String(run.stderr);
    fs.rmSync(root, { recursive: true, force: true });

    const line = (name) => {
        // A line of its own, or after the log prefix the run may put on it.
        const m = new RegExp('(?:^|\\s)' + name + ' (\\S*)\\s*$', 'm').exec(out);
        return m ? m[1].trim() : undefined;
    };
    assert(run.status === 0, 'inner run exited cleanly: ' + run.status + '\n' + out.slice(-3000));
    assert(line('SET') === 'true', 'the first page set X=41 in its shell:\n' + out.slice(-2000));
    assert(line('PARKED') === 'true', 'park() left the element with no process: ' + line('PARKED'));
    assert(line('ADOPT') === 'false,true,true,true',
           'adopt: no "nope", "main" adopted, the same pid, running: ' + line('ADOPT'));
    assert(line('TAKEN') === 'InvalidStateError', 'a key still parked is taken: ' + line('TAKEN'));
    assert(line('ORPHAN_AT_BOOT') === 'true', 'the unadopted session waits out the grace period');
    assert(line('ECHO') === 'true', 'the adopted shell kept X: echo $((X+1)) printed 42:\n' + out.slice(-2000));
    assert(line('ORPHAN_AFTER') === 'false', 'the unadopted session was ended after the grace period');
    assert(line('MAIN_AFTER') === 'true', 'the adopted one was not');
    assert(line('EXIT') === '7', 'the adopted shell\'s exit reached the new element: ' + line('EXIT'));

    console.log('terminal park/adopt across a reload OK');
}
