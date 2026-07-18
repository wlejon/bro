// Bro project manager — opens when bro is launched with no app argument.
// Manages a registry of user projects in the OS user-data dir, supports
// creating projects from skeletons, opening existing project folders, and
// drag-drop import of folders or .zip files.

// The engine menu bar is hidden by default; tooling apps like this one
// opt in explicitly.
bro.menu.show();

const fs = require('fs');
const path = require('path');
const os = require('os');
const cp = require('child_process');

const IS_WIN = process.platform === 'win32';
const IS_MAC = process.platform === 'darwin';
const EXE_SUFFIX = IS_WIN ? '.exe' : '';

// ─── Paths ────────────────────────────────────────────────────────────────

const EXE_DIR = process.env.BRO_EXE_DIR || process.cwd();
const APP_DIR = process.env.BRO_APP_DIR || process.cwd();
const BRO = path.join(EXE_DIR, 'bro' + EXE_SUFFIX);
// Skeletons live as siblings of system/projects/ — i.e. system/skeletons/.
const SKELETONS_DIR = path.join(path.dirname(APP_DIR), 'skeletons');

function userDataDir() {
    const home = os.homedir();
    if (IS_WIN) return path.join(process.env.APPDATA || path.join(home, 'AppData', 'Roaming'), 'bro');
    if (IS_MAC) return path.join(home, 'Library', 'Application Support', 'bro');
    return path.join(process.env.XDG_DATA_HOME || path.join(home, '.local', 'share'), 'bro');
}

const USER_DIR = userDataDir();
const REGISTRY_PATH = path.join(USER_DIR, 'projects.json');

function ensureUserDir() {
    try { fs.mkdirSync(USER_DIR, { recursive: true }); }
    catch (e) { console.error('mkdir userDir failed:', e); }
}

// ─── Registry ─────────────────────────────────────────────────────────────

function loadRegistry() {
    try {
        const text = fs.readFileSync(REGISTRY_PATH, 'utf-8');
        const data = JSON.parse(text);
        if (!Array.isArray(data.projects)) return [];
        return data.projects;
    } catch (e) {
        return [];
    }
}

function saveRegistry(projects) {
    ensureUserDir();
    fs.writeFileSync(REGISTRY_PATH, JSON.stringify({ projects }, null, 2), 'utf-8');
}

function isValidProjectDir(p) {
    try {
        if (!fs.statSync(p).isDirectory()) return false;
    } catch { return false; }
    return fs.existsSync(path.join(p, 'bro.json')) || fs.existsSync(path.join(p, 'index.html'));
}

function pathExists(p) {
    try { fs.statSync(p); return true; } catch { return false; }
}

let projects = loadRegistry();

function addProject(absPath) {
    absPath = normalizePath(absPath);
    const existing = projects.find(p => normalizePath(p.path) === absPath);
    if (existing) {
        existing.lastOpened = Date.now();
    } else {
        projects.push({
            path: absPath,
            name: path.basename(absPath),
            lastOpened: Date.now(),
        });
    }
    saveRegistry(projects);
}

function removeProject(absPath) {
    absPath = normalizePath(absPath);
    projects = projects.filter(p => normalizePath(p.path) !== absPath);
    saveRegistry(projects);
}

function touchProject(absPath) {
    absPath = normalizePath(absPath);
    const p = projects.find(x => normalizePath(x.path) === absPath);
    if (p) { p.lastOpened = Date.now(); saveRegistry(projects); }
}

function normalizePath(p) {
    return p.replace(/\\/g, '/').replace(/\/+$/, '');
}

// ─── Process tracking ─────────────────────────────────────────────────────

const running = new Map(); // path → { proc, tile }

function launch(project, tile) {
    const key = normalizePath(project.path);
    if (running.has(key)) {
        setStatus(`${project.name} is already running.`);
        return;
    }
    if (!pathExists(project.path)) {
        setStatus(`Project folder is missing: ${project.path}`);
        return;
    }

    if (tile) tile.classList.add('launching');
    setStatus(`Launching ${project.name}…`);

    let proc;
    try {
        proc = cp.spawn(BRO, [project.path], {
            cwd: path.dirname(project.path),
            detached: true,
        });
    } catch (e) {
        setStatus(`Failed to launch: ${e.message}`);
        if (tile) tile.classList.remove('launching');
        return;
    }

    running.set(key, { proc, tile });
    touchProject(project.path);
    render();

    setStatus(`${project.name} running (pid ${proc.pid}).`);

    proc.on('exit', (code) => {
        running.delete(key);
        render();
        setStatus(`${project.name} exited (code ${code}).`);
    });
}

// ─── UI helpers ───────────────────────────────────────────────────────────

function setStatus(msg) {
    const el = document.getElementById('status');
    if (el) el.textContent = msg || '';
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g,
        c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmtRelative(ts) {
    if (!ts) return 'never';
    const d = (Date.now() - ts) / 1000;
    if (d < 60) return 'just now';
    if (d < 3600) return `${Math.floor(d / 60)}m ago`;
    if (d < 86400) return `${Math.floor(d / 3600)}h ago`;
    return `${Math.floor(d / 86400)}d ago`;
}

let filterText = '';

function render() {
    const grid = document.getElementById('grid');
    grid.innerHTML = '';

    const sorted = [...projects].sort((a, b) => (b.lastOpened || 0) - (a.lastOpened || 0));
    const visible = sorted.filter(p => !filterText
        || p.name.toLowerCase().includes(filterText.toLowerCase())
        || p.path.toLowerCase().includes(filterText.toLowerCase()));

    if (sorted.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty-state';
        empty.innerHTML = `<h2>No projects yet</h2>
            <p>Create a new project, open an existing one, or drop a folder or .zip onto this window.</p>`;
        grid.appendChild(empty);
        setStatus('');
        return;
    }

    if (visible.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty-state';
        empty.innerHTML = `<h2>No matches</h2><p>No project matches "${escapeHtml(filterText)}".</p>`;
        grid.appendChild(empty);
        setStatus(`0 / ${sorted.length} projects`);
        return;
    }

    for (const project of visible) {
        const missing = !pathExists(project.path);
        const isRunning = running.has(normalizePath(project.path));

        const tile = document.createElement('div');
        tile.className = 'tile' + (missing ? ' missing' : '');

        const name = document.createElement('div');
        name.className = 'name';
        name.textContent = project.name;
        tile.appendChild(name);

        const p = document.createElement('div');
        p.className = 'path';
        p.textContent = project.path;
        tile.appendChild(p);

        const meta = document.createElement('div');
        meta.className = 'meta';
        meta.textContent = fmtRelative(project.lastOpened);
        if (missing) {
            const b = document.createElement('span');
            b.className = 'badge missing';
            b.textContent = 'missing';
            meta.appendChild(b);
        } else if (isRunning) {
            const b = document.createElement('span');
            b.className = 'badge running';
            b.textContent = 'running';
            meta.appendChild(b);
        }
        tile.appendChild(meta);

        const remove = document.createElement('button');
        remove.className = 'remove';
        remove.textContent = '×';
        remove.title = 'Remove from list (does not delete files)';
        remove.addEventListener('click', (ev) => {
            ev.stopPropagation();
            removeProject(project.path);
            render();
        });
        tile.appendChild(remove);

        if (!missing) {
            tile.addEventListener('click', () => launch(project, tile));
        }

        grid.appendChild(tile);
    }

    setStatus(filterText
        ? `${visible.length} / ${sorted.length} projects`
        : `${sorted.length} project${sorted.length === 1 ? '' : 's'}`);
}

// ─── Skeletons ────────────────────────────────────────────────────────────

const SKELETON_DESCRIPTIONS = {
    'ai':            'GPU probe + local LLM chat (bro.lm)',
    'blank':         'HTML/CSS/JS only',
    'canvas2d':      'Canvas 2D + animation',
    'scene3d':       '3D scene with bro.scene',
    'headless-tool': 'CLI script for bro-headless',
};

function listSkeletons() {
    try {
        const entries = fs.readdirSync(SKELETONS_DIR, { withFileTypes: true });
        return entries.filter(e => e.isDirectory()).map(e => e.name).sort();
    } catch (e) {
        console.error('listSkeletons failed:', e);
        return [];
    }
}

function copyDirRecursive(src, dst) {
    fs.mkdirSync(dst, { recursive: true });
    const entries = fs.readdirSync(src, { withFileTypes: true });
    for (const entry of entries) {
        const s = path.join(src, entry.name);
        const d = path.join(dst, entry.name);
        if (entry.isDirectory()) copyDirRecursive(s, d);
        else fs.copyFileSync(s, d);
    }
}

// ─── Zip extraction ───────────────────────────────────────────────────────

function extractZip(zipPath, destDir) {
    fs.mkdirSync(destDir, { recursive: true });
    if (IS_WIN) {
        const r = cp.spawnSync('powershell', [
            '-NoProfile', '-NonInteractive', '-Command',
            `Expand-Archive -Path '${zipPath.replace(/'/g, "''")}' -DestinationPath '${destDir.replace(/'/g, "''")}' -Force`,
        ], { encoding: 'utf-8' });
        if (r.status !== 0) {
            const msg = (r.stderr || '').trim() || `exit ${r.status}`;
            throw new Error('Expand-Archive failed: ' + msg);
        }
    } else {
        let r = cp.spawnSync('unzip', ['-o', zipPath, '-d', destDir], { encoding: 'utf-8' });
        if (r.status !== 0) {
            // Fallback to bsdtar (common on macOS).
            r = cp.spawnSync('tar', ['-xf', zipPath, '-C', destDir], { encoding: 'utf-8' });
            if (r.status !== 0) {
                const msg = (r.stderr || '').trim() || `exit ${r.status}`;
                throw new Error('unzip/tar failed: ' + msg);
            }
        }
    }
    // If the zip wraps everything in a single subfolder, descend into it so
    // bro.json lives at the project root.
    const top = fs.readdirSync(destDir);
    if (top.length === 1) {
        const inner = path.join(destDir, top[0]);
        if (fs.statSync(inner).isDirectory() && isValidProjectDir(inner)) return inner;
    }
    return destDir;
}

// ─── New-project modal ────────────────────────────────────────────────────

const modalNew = document.getElementById('modal-new');
const newName = document.getElementById('new-name');
const newParent = document.getElementById('new-parent');
const newError = document.getElementById('new-error');
let selectedSkeleton = null;

function openNewModal() {
    selectedSkeleton = null;
    newName.value = '';
    newParent.value = '';
    newError.textContent = '';

    const list = document.getElementById('skeleton-list');
    list.innerHTML = '';
    const skels = listSkeletons();
    if (skels.length === 0) {
        list.innerHTML = '<div class="error">No skeletons found at ' + escapeHtml(SKELETONS_DIR) + '</div>';
    }
    for (const name of skels) {
        const el = document.createElement('div');
        el.className = 'skel';
        el.innerHTML = `<div class="skel-name">${escapeHtml(name)}</div>
                        <div class="skel-desc">${escapeHtml(SKELETON_DESCRIPTIONS[name] || '')}</div>`;
        el.addEventListener('click', () => {
            selectedSkeleton = name;
            list.querySelectorAll('.skel').forEach(s => s.classList.remove('selected'));
            el.classList.add('selected');
        });
        list.appendChild(el);
    }
    if (skels.length > 0) {
        selectedSkeleton = skels[0];
        list.firstChild.classList.add('selected');
    }

    modalNew.classList.remove('hidden');
    newName.focus();
}

function closeNewModal() { modalNew.classList.add('hidden'); }

document.getElementById('btn-new').addEventListener('click', openNewModal);
document.getElementById('new-cancel').addEventListener('click', closeNewModal);

document.getElementById('new-parent-pick').addEventListener('click', () => {
    const picked = showOpenFolderDialog(os.homedir(), false);
    if (picked && picked.length > 0) newParent.value = picked[0];
});

document.getElementById('new-create').addEventListener('click', () => {
    newError.textContent = '';
    const name = newName.value.trim();
    const parent = newParent.value.trim();
    if (!name) { newError.textContent = 'Name required.'; return; }
    if (!/^[A-Za-z0-9_\-. ]+$/.test(name)) {
        newError.textContent = 'Name has invalid characters.';
        return;
    }
    if (!parent) { newError.textContent = 'Pick a parent folder.'; return; }
    if (!selectedSkeleton) { newError.textContent = 'Pick a skeleton.'; return; }

    const dest = path.join(parent, name);
    if (fs.existsSync(dest)) { newError.textContent = `${dest} already exists.`; return; }

    const src = path.join(SKELETONS_DIR, selectedSkeleton);
    if (!fs.existsSync(src)) { newError.textContent = 'Skeleton not found: ' + src; return; }

    try {
        copyDirRecursive(src, dest);
    } catch (e) {
        newError.textContent = 'Copy failed: ' + e.message;
        return;
    }

    addProject(dest);
    closeNewModal();
    render();
    launch({ path: dest, name }, null);
});

// ─── Open existing ────────────────────────────────────────────────────────

document.getElementById('btn-open').addEventListener('click', () => {
    const picked = showOpenFolderDialog(os.homedir(), false);
    if (!picked || picked.length === 0) return;
    const folder = picked[0];
    if (!isValidProjectDir(folder)) {
        setStatus(`Not a bro project (no bro.json or index.html): ${folder}`);
        return;
    }
    addProject(folder);
    render();
    launch({ path: folder, name: path.basename(folder) }, null);
});

// ─── Drag and drop ────────────────────────────────────────────────────────

const dropOverlay = document.getElementById('drop-overlay');
let dragDepth = 0;

document.body.addEventListener('dragenter', (e) => {
    e.preventDefault();
    dragDepth++;
    dropOverlay.classList.remove('hidden');
});

document.body.addEventListener('dragover', (e) => {
    e.preventDefault();
});

document.body.addEventListener('dragleave', (e) => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (dragDepth === 0) dropOverlay.classList.add('hidden');
});

document.body.addEventListener('drop', (e) => {
    e.preventDefault();
    dragDepth = 0;
    dropOverlay.classList.add('hidden');

    const files = (e.dataTransfer && e.dataTransfer.files) || [];
    if (files.length === 0) {
        setStatus('Drop ignored: no files.');
        return;
    }
    for (let i = 0; i < files.length; i++) {
        handleDroppedPath(files[i].path);
    }
});

function handleDroppedPath(p) {
    if (!p) return;
    let st;
    try { st = fs.statSync(p); }
    catch (e) { setStatus(`Cannot read ${p}: ${e.message}`); return; }

    if (st.isDirectory()) {
        if (!isValidProjectDir(p)) {
            setStatus(`Not a bro project (no bro.json or index.html): ${p}`);
            return;
        }
        addProject(p);
        render();
        launch({ path: p, name: path.basename(p) }, null);
        return;
    }

    if (/\.zip$/i.test(p)) {
        setStatus(`Pick where to extract ${path.basename(p)}…`);
        const picked = showOpenFolderDialog(os.homedir(), false);
        if (!picked || picked.length === 0) { setStatus('Extract cancelled.'); return; }
        const baseName = path.basename(p, path.extname(p));
        const dest = path.join(picked[0], baseName);
        if (fs.existsSync(dest)) { setStatus(`Destination exists: ${dest}`); return; }
        try {
            const projectRoot = extractZip(p, dest);
            if (!isValidProjectDir(projectRoot)) {
                setStatus(`Extracted, but no bro.json/index.html found in ${projectRoot}`);
                return;
            }
            addProject(projectRoot);
            render();
            launch({ path: projectRoot, name: path.basename(projectRoot) }, null);
        } catch (e) {
            setStatus('Extract failed: ' + e.message);
        }
        return;
    }

    setStatus(`Drop ignored (not a folder or .zip): ${p}`);
}

// ─── Filter ───────────────────────────────────────────────────────────────

const filterInput = document.getElementById('filter');
filterInput.addEventListener('input', () => {
    filterText = filterInput.value.trim();
    render();
});
filterInput.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && filterInput.value) {
        filterInput.value = '';
        filterText = '';
        render();
    }
});

// ─── Init ─────────────────────────────────────────────────────────────────

ensureUserDir();
const hint = document.getElementById('user-dir-hint');
if (hint) hint.textContent = USER_DIR;

window.addEventListener('beforeunload', () => {
    // Detached children outlive us by design; nothing to clean up.
});

render();
