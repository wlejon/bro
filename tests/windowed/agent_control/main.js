// The agent control socket of a windowed bro, end to end through bro-ctl
// (docs/agent-control.md): two processes of one app serving side by side,
// bro-ctl picking each by pid and refusing to guess between them by app
// name, and a windowed `record` written out — video.mp4 included when ffmpeg
// is on the PATH, made by the command bro-ctl hands the platform's shell.
//
// The two bros are children of this one, running ./child with BRO_CONTROL=1
// on the same video driver as this test (SDL offscreen, or its hidden-window
// stand-in where the GPU driver has no headless surface).

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const isWin = os.platform() === 'win32';
const exeDir = path.dirname(process.execPath);
const broExe = process.execPath;
const ctlExe = path.join(exeDir, isWin ? 'bro-ctl.exe' : 'bro-ctl');
const childApp = path.join(bro.appDir, 'child');

function text(v) {
    if (v == null) return '';
    if (typeof v === 'string') return v;
    return new TextDecoder().decode(v instanceof ArrayBuffer ? new Uint8Array(v) : v);
}

function ctl(args) {
    const r = cp.spawnSync(ctlExe, args, { stdio: 'pipe' });
    return { status: r.status, out: text(r.stdout), err: text(r.stderr) };
}

// An endpoint's address ends in its name: <dir>/<name>.sock, or on Windows
// the pipe \\.\pipe\bro-control-<SID>-<name>.
function endsWithEndpoint(address, name) {
    return address.endsWith(isWin ? '-' + name : '/' + name + '.sock');
}

// The endpoints `bro-ctl list` printed: { live: [addresses], stale: [...] }.
function parseList(listed) {
    const out = { live: [], stale: [] };
    for (const line of listed.split('\n').map((l) => l.trim()).filter((l) => l)) {
        if (line.endsWith('(stale)')) out.stale.push(line.replace(/\s*\(stale\)$/, ''));
        else out.live.push(line);
    }
    return out;
}

function listedName(listed, name, liveOnly) {
    const l = parseList(listed);
    return (liveOnly ? l.live : l.live.concat(l.stale)).some((a) => endsWithEndpoint(a, name));
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

const children = [];

// A child that did not come up: how it ended, and the end of its log.
function childReport(c) {
    let log = '';
    try {
        const files = fs.readdirSync(c.dir).filter((f) => /^bro.*\.log$/.test(f));
        for (const f of files) log += fs.readFileSync(path.join(c.dir, f), 'utf-8').split('\n').slice(-40).join('\n');
    } catch (e) {
        log = '(no log: ' + e + ')';
    }
    return `\n  child pid ${c.proc.pid} exit ${c.exit === null ? 'still running' : c.exit}; its log:\n${log}`;
}

async function run() {
    if (!fs.existsSync(ctlExe)) {
        skipTest('bro-ctl is not built beside ' + broExe);
        return;
    }
    const env = Object.assign({}, process.env, { BRO_CONTROL: '1' });
    for (let i = 0; i < 2; i++) {
        const dir = path.join(os.tmpdir(), `bro-agent-control-${process.pid}-${i}`);
        fs.mkdirSync(dir, { recursive: true });
        const child = { proc: cp.spawn(broExe, ['--no-splash', childApp], { env, cwd: dir }), dir, exit: null };
        child.proc.on('exit', (code, signal) => (child.exit = `${code}/${signal}`));
        children.push(child);
    }
    const pids = children.map((c) => c.proc.pid);
    assert(pids.every((p) => p > 0), `both children started: ${pids}`);

    // Each serves <app>-<pid>: the names differ, so neither takes the other's.
    const names = pids.map((p) => 'child-' + p);
    let listed = '';
    const waitStart = Date.now();
    for (const until = Date.now() + 60000; Date.now() < until; await sleep(200)) {
        listed = ctl(['list']).out;
        if (names.every((n) => listedName(listed, n, true))) break;
        if (children.some((c) => c.exit !== null)) break;
    }
    children.forEach((c, i) => {
        const ok = listedName(listed, names[i], true);
        assert(ok, `bro-ctl list shows ${names[i]} serving: ${listed}` + (ok ? '' : childReport(c)));
    });
    if (!names.every((n) => listedName(listed, n, true))) return;
    console.log(`agent control: both children serving ${Date.now() - waitStart} ms after spawn`);

    // -s <pid> and -s <app>-<pid> each reach their own process.
    for (const p of pids) {
        const byPid = ctl(['-s', String(p), 'info']);
        assert(byPid.status === 0, `bro-ctl -s ${p} info succeeds: ${byPid.err}`);
        const info = JSON.parse(byPid.out);
        assert(info.pid === p, `-s ${p} reached pid ${info.pid}`);
        assert(info.mode === 'windowed', `the child is windowed: ${info.mode}`);
        assert(endsWithEndpoint(info.socket, 'child-' + p), `the socket is named after app and pid: ${info.socket}`);
        const byName = ctl(['-s', 'child-' + p, 'info']);
        assert(byName.status === 0 && JSON.parse(byName.out).pid === p, `-s child-${p} reaches ${p}`);
    }

    // The app name alone is ambiguous with two running: bro-ctl says so.
    const ambiguous = ctl(['-s', 'child', 'info']);
    assert(ambiguous.status === 2, `-s child with two running exits 2, got ${ambiguous.status}`);
    assert(names.every((n) => ambiguous.err.includes(n)), `and lists both: ${ambiguous.err}`);

    // record, windowed: frames off the swapchain readback, then (with
    // ffmpeg) video.mp4 through the platform shell, from a path with a space.
    const recDir = path.join(children[0].dir, 'rec dir');
    const rec = ctl(['-s', String(pids[0]), 'record', '1', recDir, '--scale=0.5']);
    assert(rec.status === 0, `record succeeds: ${rec.err}${rec.out}`);
    const reply = JSON.parse(rec.out);
    assert(reply.frames >= 10, `record kept the animating frames: ${reply.frames}`);
    assert(reply.changed >= 5, `and saw them change: ${reply.changed}`);
    for (const f of ['frames.ffconcat', 'contact.png', 'timeline.json', 'summary.txt'])
        assert(fs.existsSync(path.join(recDir, f)), `record wrote ${f}`);
    assert(fs.existsSync(path.join(recDir, 'frames', '00000.png')), 'record wrote the frames');
    const timeline = JSON.parse(fs.readFileSync(path.join(recDir, 'timeline.json'), 'utf-8'));
    assert(timeline.refreshMs > 0, `a refresh period to measure gaps against: ${timeline.refreshMs}`);
    const ffmpeg = cp.spawnSync('ffmpeg', ['-version'], { stdio: 'pipe' });
    if (ffmpeg.status === 0) {
        assert(fs.existsSync(path.join(recDir, 'video.mp4')), 'bro-ctl made video.mp4 with ffmpeg');
        assert(typeof reply.video === 'string' && reply.video.endsWith('video.mp4'),
               `the reply names the video: ${reply.video}`);
    } else {
        console.log('agent control: ffmpeg not on PATH, video.mp4 not checked');
    }

    // A killed bro leaves its socket file (POSIX; a pipe goes with its
    // process); the next bro to start control removes it, as brolink sweeps
    // every dead server's socket from the directory it binds in.
    const dead = children[0].proc;
    const exited = new Promise((resolve) => dead.on('exit', resolve));
    dead.kill('SIGKILL');
    await exited;
    const thirdChild = { proc: cp.spawn(broExe, ['--no-splash', childApp], { env, cwd: children[1].dir }),
                         dir: children[1].dir, exit: null };
    thirdChild.proc.on('exit', (code, signal) => (thirdChild.exit = `${code}/${signal}`));
    children.push(thirdChild);
    const thirdName = 'child-' + thirdChild.proc.pid;
    for (const until = Date.now() + 60000; Date.now() < until; await sleep(200)) {
        listed = ctl(['list']).out;
        if (listedName(listed, thirdName, true) || thirdChild.exit !== null) break;
    }
    const thirdUp = listedName(listed, thirdName, true);
    assert(thirdUp, `the third bro serves ${thirdName}: ${listed}` + (thirdUp ? '' : childReport(thirdChild)));
    assert(!listedName(listed, names[0], false), `the killed bro's ${names[0]} was removed: ${listed}`);
    assert(listedName(listed, names[1], true), `the live one's stays: ${listed}`);

    console.log(`windowed agent control: ${names.join(', ')}; recorded ${reply.frames} frames`);
}

function cleanup() {
    for (const c of children) {
        try { c.proc.kill('SIGKILL'); } catch (e) {}
    }
}

const watchdog = setTimeout(() => {
    assert(false, 'windowed agent control test timed out');
    cleanup();
    window.close();
}, 90000);
run().catch((e) => assert(false, 'uncaught: ' + e + '\n' + (e && e.stack))).finally(() => {
    clearTimeout(watchdog);
    cleanup();
    // Children killed, their scratch directories go (a killed one's socket
    // is removed by the next bro that starts control).
    setTimeout(() => {
        for (const c of children) {
            try { fs.rmSync(c.dir, { recursive: true, force: true }); } catch (e) {}
        }
        window.close();
    }, 500);
});
