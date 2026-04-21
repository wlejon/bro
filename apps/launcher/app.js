// Bro launcher — grid of installed apps. Clicking a tile spawns a detached
// bro child process. Apps that declare a server entry in apps.json have
// their server script run in-process as a Worker; the worker is terminated
// when the client process exits.

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const IS_WIN = process.platform === 'win32';
const EXE_SUFFIX = IS_WIN ? '.exe' : '';

// BRO_EXE_DIR is set by bro/main.cpp before the engine starts.
const EXE_DIR = process.env.BRO_EXE_DIR || process.cwd();
const BRO = path.join(EXE_DIR, 'bro' + EXE_SUFFIX);

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

// The app that pasted images will overwrite the thumbnail for. Set to the
// most recently launched app, or whichever pill the user clicks.
let pasteTargetDir = null;

function setPasteTarget(dir) {
    pasteTargetDir = running.has(dir) ? dir : null;
    updateRunningStrip();
}

function updateRunningStrip() {
    const strip = document.getElementById('running-strip');
    strip.innerHTML = '';
    for (const [dir, entry] of running) {
        const pill = document.createElement('div');
        pill.className = 'running-pill';
        if (dir === pasteTargetDir) pill.classList.add('selected');
        pill.title = 'Click to target Ctrl+V thumbnail update';
        pill.innerHTML = `<span class="dot"></span><span>${entry.app.title}</span>`;
        pill.addEventListener('click', () => setPasteTarget(dir));
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

    // Host the app's server (if declared) in a Worker inside this launcher
    // process. Uses the worker-scoped bro.net / bro.server / bro.ai.game
    // bindings — same JS surface the old bro-server subprocess exposed.
    let serverWorker = null;
    if (app.server) {
        try {
            const serverScript = path.join(app.appPath, app.server.script);
            serverWorker = new Worker(serverScript);
            serverWorker.onmessage = (e) => {
                // Server scripts don't normally postMessage back, but surface
                // anything that arrives so debugging isn't opaque.
                console.log(`[${app.dir} server]`, e.data);
            };
        } catch (e) {
            console.error('server worker failed:', e);
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
        if (serverWorker) serverWorker.terminate();
        tile.classList.remove('launching');
        return;
    }

    running.set(app.dir, { app, client: clientProc, server: serverWorker, tile });
    pasteTargetDir = app.dir;
    updateRunningStrip();
    setStatus(`${app.title} running (pid ${clientProc.pid}). Ctrl+V to paste a new thumbnail.`);

    clientProc.on('exit', (code) => {
        const entry = running.get(app.dir);
        if (!entry) return;
        if (entry.server) entry.server.terminate();
        running.delete(app.dir);
        if (pasteTargetDir === app.dir) {
            pasteTargetDir = running.keys().next().value || null;
        }
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

    // Per-tile background-image rules live in thumbnails.css.
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

// ─── Paste-to-update-thumbnail ─────────────────────────────────────────────
// Ctrl+V from an external screenshot (Snip & Sketch on Windows, ⌘⇧4 on mac)
// overwrites the thumbnail for the currently-targeted app.

async function writeThumbnailFromClipboard(e) {
    if (!pasteTargetDir) {
        setStatus('Paste ignored: no app targeted. Launch one or click its running pill.');
        return;
    }
    const items = (e.clipboardData && e.clipboardData.items) || [];
    let file = null;
    // Prefer PNG; BMP is accepted but clipboard-as-BMP is an edge case.
    for (const it of items) {
        if (it.kind === 'file' && it.type === 'image/png') { file = it.getAsFile(); break; }
    }
    if (!file) {
        for (const it of items) {
            if (it.kind === 'file' && it.type === 'image/bmp') { file = it.getAsFile(); break; }
        }
    }
    if (!file) {
        setStatus('Paste ignored: clipboard has no image.');
        return;
    }

    const dir = pasteTargetDir;
    const ext = file.type === 'image/png' ? 'png' : 'bmp';
    const outPath = path.join(APPS_ROOT, 'launcher', 'thumbnails', dir + '.' + ext);

    const buf = new Uint8Array(await file.arrayBuffer());
    fs.writeFileSync(outPath, buf);

    // Bust the CSS background-image cache for just this tile by writing an
    // inline style on the thumb div with a timestamped URL.
    const entry = running.get(dir);
    const thumb = entry && entry.tile.querySelector('.thumb');
    const stamp = Date.now();
    const relUrl = 'thumbnails/' + dir + '.' + ext + '?v=' + stamp;
    if (thumb) {
        thumb.style.backgroundImage = 'url(' + relUrl + ')';
        thumb.textContent = '';
    }
    // Also update any non-running tile for this dir. (NodeList is not
    // iterable in bro yet — use indexed access.)
    const allTiles = document.querySelectorAll('.thumb[data-app="' + dir + '"]');
    for (let i = 0; i < allTiles.length; i++) {
        const t = allTiles[i];
        t.style.backgroundImage = 'url(' + relUrl + ')';
        t.textContent = '';
    }
    setStatus(`Updated thumbnail for ${entry ? entry.app.title : dir} (${buf.length} bytes).`);
    e.preventDefault();
}

document.addEventListener('paste', (e) => {
    writeThumbnailFromClipboard(e).catch(err => {
        console.error('paste failed:', err);
        setStatus('Paste failed: ' + err.message);
    });
});

window.addEventListener('beforeunload', () => {
    for (const entry of running.values()) {
        if (entry.server) entry.server.terminate();
    }
});

render();
