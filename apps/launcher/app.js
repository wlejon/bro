// Bro launcher — grid of installed apps. Clicking a tile spawns a detached
// bro child process. Apps that declare a server entry in apps.json also spawn
// a bro-server child; the server is killed when the client process exits.

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const IS_WIN = process.platform === 'win32';
const EXE_SUFFIX = IS_WIN ? '.exe' : '';

// BRO_EXE_DIR is set by bro/main.cpp before the engine starts.
const EXE_DIR = process.env.BRO_EXE_DIR || process.cwd();
const BRO = path.join(EXE_DIR, 'bro' + EXE_SUFFIX);
const BRO_SERVER = path.join(EXE_DIR, 'bro-server' + EXE_SUFFIX);

// Locate the apps directory. In release, apps/ sits next to bro.exe. In dev
// (running `bro apps/launcher` from the project root), it's under cwd.
function findAppsRoot() {
    const candidates = [
        path.join(EXE_DIR, 'apps'),
        path.join(process.cwd(), 'apps'),
    ];
    for (const c of candidates) {
        if (fs.existsSync(path.join(c, 'launcher', 'apps.json'))) return c;
    }
    return candidates[0];
}
const APPS_ROOT = findAppsRoot();
// cwd for spawned children — lets them resolve "apps/<dir>" consistently.
const SPAWN_CWD = path.dirname(APPS_ROOT);

// ─── Load manifest + per-app bro.json ──────────────────────────────────────

function readJSON(p) {
    try { return JSON.parse(fs.readFileSync(p, 'utf8')); }
    catch (e) { return null; }
}

function loadApps() {
    // 'apps.json' and '../<dir>/bro.json' resolve against the launcher app dir
    // via brokit's fs base-path mechanism.
    const manifest = readJSON('apps.json');
    if (!manifest || !Array.isArray(manifest.apps)) return [];
    return manifest.apps.map(entry => {
        const dir = entry.dir;
        const cfg = readJSON('../' + dir + '/bro.json') || {};
        return {
            dir,
            appPath: path.join(APPS_ROOT, dir),
            title: entry.title || cfg.title || dir,
            width: entry.width || cfg.width || 1280,
            height: entry.height || cfg.height || 720,
            server: entry.server || null,
            thumbnailRel: 'thumbnails/' + dir + '.png',
        };
    });
}

// ─── Process tracking ──────────────────────────────────────────────────────

const running = new Map();

function updateRunningStrip() {
    const strip = document.getElementById('running-strip');
    strip.innerHTML = '';
    for (const [dir, entry] of running) {
        const pill = document.createElement('div');
        pill.className = 'running-pill';
        pill.innerHTML = `<span class="dot"></span><span>${entry.app.title}</span>`;
        const close = document.createElement('button');
        close.textContent = '×';
        close.title = 'Stop';
        close.addEventListener('click', (ev) => {
            ev.stopPropagation();
            stopApp(dir);
        });
        pill.appendChild(close);
        strip.appendChild(pill);
    }
}

function setStatus(msg) {
    const el = document.getElementById('status');
    if (el) el.textContent = msg || '';
}

function launchApp(app, tile) {
    if (running.has(app.dir)) {
        setStatus(`${app.title} is already running.`);
        return;
    }

    tile.classList.add('launching');
    setStatus(`Launching ${app.title}…`);

    let serverProc = null;
    if (app.server) {
        try {
            serverProc = cp.spawn(BRO_SERVER, [app.appPath, app.server.script], {
                cwd: SPAWN_CWD,
            });
            serverProc.on('exit', () => {
                const entry = running.get(app.dir);
                if (entry && entry.server === serverProc) entry.server = null;
            });
        } catch (e) {
            console.error('server spawn failed:', e);
            setStatus(`Server failed for ${app.title}: ${e.message}`);
            tile.classList.remove('launching');
            return;
        }
    }

    let clientProc;
    try {
        clientProc = cp.spawn(BRO, [app.appPath], { cwd: SPAWN_CWD });
    } catch (e) {
        console.error('client spawn failed:', e);
        setStatus(`Failed to launch ${app.title}: ${e.message}`);
        if (serverProc) serverProc.kill();
        tile.classList.remove('launching');
        return;
    }

    running.set(app.dir, { app, client: clientProc, server: serverProc, tile });
    updateRunningStrip();
    setStatus(`${app.title} running (pid ${clientProc.pid}).`);

    clientProc.on('exit', (code) => {
        const entry = running.get(app.dir);
        if (!entry) return;
        if (entry.server) entry.server.kill();
        running.delete(app.dir);
        tile.classList.remove('launching');
        updateRunningStrip();
        setStatus(`${app.title} exited (code ${code}).`);
    });

    setTimeout(() => tile.classList.remove('launching'), 800);
}

function stopApp(dir) {
    const entry = running.get(dir);
    if (!entry) return;
    entry.client.kill();
}

// ─── UI ────────────────────────────────────────────────────────────────────

function render() {
    const grid = document.getElementById('grid');
    grid.innerHTML = '';

    const apps = loadApps();

    // Per-tile background-image rules live in thumbnails.css, generated by
    // generate_thumbnails.js alongside the PNGs.
    if (apps.length === 0) {
        grid.innerHTML = '<div style="color:#8a8f97">No apps found.</div>';
        return;
    }

    for (const app of apps) {
        const tile = document.createElement('div');
        tile.className = 'tile';

        const thumb = document.createElement('div');
        thumb.className = 'thumb';
        thumb.setAttribute('data-app', app.dir);
        if (!fs.existsSync(app.thumbnailRel)) {
            thumb.textContent = 'no preview';
        }

        const meta = document.createElement('div');
        meta.className = 'meta';
        const title = document.createElement('div');
        title.className = 'title';
        title.textContent = app.title;
        const sub = document.createElement('div');
        sub.className = 'sub';
        sub.textContent = `${app.width}×${app.height} · apps/${app.dir}`;
        meta.appendChild(title);
        meta.appendChild(sub);
        if (app.server) {
            const badge = document.createElement('span');
            badge.className = 'badge';
            badge.textContent = 'server';
            meta.appendChild(badge);
        }

        tile.appendChild(thumb);
        tile.appendChild(meta);
        tile.addEventListener('click', () => launchApp(app, tile));
        grid.appendChild(tile);
    }

    setStatus(`${apps.length} apps · ${EXE_DIR}`);
}

window.addEventListener('beforeunload', () => {
    for (const entry of running.values()) {
        if (entry.server) entry.server.kill();
    }
});

render();
