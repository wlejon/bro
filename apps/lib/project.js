// project.js — project-level save/load for bro apps.
//
// Bundles are directories (Godot / Logic / Final Cut style):
//
//   my-scene.bro/
//   ├── project.json   { bro_project, app, schema, created, modified, data }
//   └── assets/        (lazy — only if the app stores binary sidecars)
//
// Apps own the domain (serialize + deserialize); this library owns the
// plumbing: dialogs, schema versioning + forward migrations, dirty tracking
// wired to apps/lib/history, autosave, and "unsaved changes" prompting.
//
// Usage:
//   <script src="../lib/project.js"></script>
//   const proj = new Project({
//       app:        'scene-editor',
//       schema:     2,
//       serialize:   () => ({ primitives: registry.primitives.map(...) }),
//       deserialize: (data) => { registry.clear(); data.primitives.forEach(...); },
//       onNew:       () => { registry.clear(); registry.create(defaultBox); },
//       history,                                       // auto-dirty on record
//       migrations:  { 1: dataV1 => ({...dataV1, color: '#fff'}) },
//       promptDirty: () => confirm('Unsaved changes — continue?'),
//   });
//   proj.save(); proj.saveAs(); proj.open(); proj.new();
//   proj.autosaveEvery(30_000);

(function (global) {
    'use strict';

    // Container format version — bumped if the project.json envelope itself
    // changes (e.g. assets index, binary sidecar layout). App-level schema
    // evolves independently via `opts.schema` + `opts.migrations`.
    const CONTAINER_VERSION = 1;

    function nowISO() {
        return new Date().toISOString();
    }

    function Project(opts) {
        if (!opts || typeof opts.app !== 'string' ||
            typeof opts.serialize !== 'function' ||
            typeof opts.deserialize !== 'function') {
            throw new Error('Project: requires { app, serialize, deserialize }');
        }
        this._app          = opts.app;
        this._schema       = opts.schema != null ? opts.schema : 1;
        this._serialize    = opts.serialize;
        this._deserialize  = opts.deserialize;
        this._onNew        = opts.onNew || null;
        this._migrations   = opts.migrations || {};
        this._fileExt      = opts.fileExt || 'bro';
        this._promptDirty  = opts.promptDirty || null;
        this._onBeforeLoad = opts.onBeforeLoad || null;
        this._history      = opts.history || null;

        this._path         = null;
        this._createdAt    = null;
        this._dirty        = false;
        this._autosaveMs   = 0;
        this._autosaveHandle = null;
        // Set while load/new/save is swapping state — suppresses history-
        // driven dirty flags so the freshly-loaded state isn't immediately
        // marked dirty by the deserialize path replaying commands.
        this._loading      = false;
        this._listeners    = {};

        // Auto-dirty from history. Only records made by user actions should
        // mark the project dirty; loads that re-enter deserialize shouldn't.
        if (this._history && typeof this._history.on === 'function') {
            const self = this;
            this._history.on('record', () => {
                if (!self._loading) self.markDirty();
            });
        }
    }

    Project.prototype = {

        // ---- state ------------------------------------------------------

        get path() { return this._path; },
        get name() {
            if (!this._path) return 'Untitled';
            return require('path').basename(this._path);
        },
        get schema() { return this._schema; },
        get app()    { return this._app; },

        isDirty()   { return this._dirty; },
        markDirty() {
            if (this._dirty) return;
            this._dirty = true;
            this._emit('dirty', { dirty: true });
            this._emit('change', this);
        },
        markClean() {
            if (!this._dirty) return;
            this._dirty = false;
            this._emit('dirty', { dirty: false });
            this._emit('change', this);
        },

        // ---- events -----------------------------------------------------
        //
        // Events: 'dirty' ({dirty}), 'saved' ({path}), 'loaded' ({path}),
        // 'new' ({}), 'change' (any of the above also emits 'change').

        on(event, fn) {
            (this._listeners[event] ||= []).push(fn);
            return () => this.off(event, fn);
        },
        off(event, fn) {
            const arr = this._listeners[event];
            if (!arr) return;
            const i = arr.indexOf(fn);
            if (i >= 0) arr.splice(i, 1);
        },
        _emit(event, payload) {
            const arr = this._listeners[event];
            if (!arr) return;
            const snap = arr.slice();
            for (let i = 0; i < snap.length; i++) snap[i](payload, this);
        },

        // ---- save -------------------------------------------------------

        // Save to the current path. Returns false if no path is set — caller
        // should fall back to saveAs().
        save() {
            if (!this._path) return false;
            return this.saveTo(this._path);
        },

        // Prompt for a location via native save dialog, then save. Appends
        // the configured file extension if missing ("my-scene" → "my-scene.bro").
        // Returns false if the dialog was cancelled.
        saveAs() {
            if (typeof showSaveFileDialog !== 'function') {
                throw new Error('Project.saveAs: showSaveFileDialog unavailable');
            }
            const filter = `Bro Project|${this._fileExt}`;
            const defaultName = this._path
                ? require('path').basename(this._path)
                : 'project.' + this._fileExt;
            const picked = showSaveFileDialog(filter, defaultName);
            if (!picked) return false;
            return this.saveTo(this._resolveSavePath(picked));
        },

        // Write the bundle to `dirPath` without prompting. Creates the
        // directory if missing; writes project.json via tmp-then-rename so a
        // mid-write crash can't leave the file truncated.
        saveTo(dirPath) {
            const fs = require('fs');
            const path = require('path');
            if (!this._createdAt) this._createdAt = nowISO();
            const envelope = {
                bro_project: CONTAINER_VERSION,
                app:      this._app,
                schema:   this._schema,
                created:  this._createdAt,
                modified: nowISO(),
                data:     this._serialize(),
            };
            fs.mkdirSync(dirPath, { recursive: true });
            const target = path.join(dirPath, 'project.json');
            const tmp    = target + '.tmp';
            fs.writeFileSync(tmp, JSON.stringify(envelope, null, 2), 'utf8');
            // Windows rename won't overwrite; remove the old file first.
            if (fs.existsSync(target)) fs.unlinkSync(target);
            fs.renameSync(tmp, target);
            this._path = dirPath;
            this._dirty = false;
            this._emit('saved', { path: dirPath });
            this._emit('dirty', { dirty: false });
            this._emit('change', this);
            return true;
        },

        // ---- open -------------------------------------------------------

        // Prompt via folder dialog, then load. Runs the unsaved-changes
        // gate via promptDirty if configured.
        open() {
            if (!this._promptIfDirty()) return false;
            if (typeof showOpenFolderDialog !== 'function') {
                throw new Error('Project.open: showOpenFolderDialog unavailable');
            }
            const picked = showOpenFolderDialog();
            if (!picked || picked.length === 0) return false;
            return this.openPath(picked[0]);
        },

        // Load a bundle from `dirPath`. Validates app id, applies schema
        // migrations, clears history (an old undo stack makes no sense for a
        // freshly loaded file), then calls deserialize.
        openPath(dirPath) {
            const fs = require('fs');
            const path = require('path');
            if (!fs.existsSync(dirPath)) {
                throw new Error(`Project.openPath: no such path: ${dirPath}`);
            }
            if (!fs.statSync(dirPath).isDirectory()) {
                throw new Error(`Project.openPath: not a directory: ${dirPath}`);
            }
            const projectFile = path.join(dirPath, 'project.json');
            if (!fs.existsSync(projectFile)) {
                throw new Error(`Project.openPath: no project.json in ${dirPath}`);
            }
            const envelope = JSON.parse(fs.readFileSync(projectFile, 'utf8'));
            if (envelope.app !== this._app) {
                throw new Error(
                    `Project.openPath: expected app "${this._app}", got "${envelope.app}"`);
            }
            const data = this._migrate(envelope.data, envelope.schema);

            this._loading = true;
            try {
                if (this._onBeforeLoad) this._onBeforeLoad();
                if (this._history && typeof this._history.clear === 'function') {
                    this._history.clear();
                }
                this._deserialize(data);
            } finally {
                this._loading = false;
            }
            this._path      = dirPath;
            this._createdAt = envelope.created || null;
            this._dirty     = false;
            this._emit('loaded', { path: dirPath });
            this._emit('dirty', { dirty: false });
            this._emit('change', this);
            return true;
        },

        // ---- new --------------------------------------------------------

        // Reset to a blank state via `onNew`. Runs the dirty-gate first.
        // Clears history and the current path.
        new() {
            if (!this._promptIfDirty()) return false;
            this._loading = true;
            try {
                if (this._history && typeof this._history.clear === 'function') {
                    this._history.clear();
                }
                if (this._onNew) this._onNew();
            } finally {
                this._loading = false;
            }
            this._path      = null;
            this._createdAt = null;
            this._dirty     = false;
            this._emit('new', {});
            this._emit('dirty', { dirty: false });
            this._emit('change', this);
            return true;
        },

        // ---- autosave ---------------------------------------------------

        // Periodic save while there's a current path and state is dirty.
        // 0 disables. Safe to call repeatedly; replaces any prior timer.
        autosaveEvery(ms) {
            if (this._autosaveHandle) {
                clearInterval(this._autosaveHandle);
                this._autosaveHandle = null;
            }
            this._autosaveMs = ms;
            if (ms > 0) {
                const self = this;
                this._autosaveHandle = setInterval(() => {
                    if (self._dirty && self._path) self.save();
                }, ms);
            }
        },
        autosaveActive() { return !!this._autosaveHandle; },

        // ---- internals --------------------------------------------------

        _migrate(data, fromSchema) {
            if (fromSchema == null) fromSchema = 1;
            if (fromSchema > this._schema) {
                throw new Error(
                    `Project: file schema ${fromSchema} is newer than app (${this._schema})`);
            }
            while (fromSchema < this._schema) {
                const migrator = this._migrations[fromSchema];
                if (typeof migrator !== 'function') {
                    throw new Error(
                        `Project: no migration from schema ${fromSchema} to ${fromSchema + 1}`);
                }
                data = migrator(data);
                fromSchema++;
            }
            return data;
        },

        _promptIfDirty() {
            if (!this._dirty) return true;
            if (this._promptDirty) return !!this._promptDirty();
            return true;    // no prompt configured — trust the caller
        },

        _resolveSavePath(picked) {
            const ext = '.' + this._fileExt;
            return picked.endsWith(ext) ? picked : picked + ext;
        },
    };

    global.Project = Project;
})(typeof window !== 'undefined' ? window : globalThis);
