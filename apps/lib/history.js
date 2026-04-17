// history.js — undo/redo command stack for bro apps.
//
// Pure-JS command pattern: apps opt in at each mutation site via do() /
// record() / transaction(). No proxying, no engine coupling — the stack
// just holds {doFn, undoFn} pairs and invokes them.
//
// Usage:
//   <script src="../lib/history.js"></script>
//   const h = new History({ limit: 200, onChange: () => updateEditMenu() });
//
//   // Simple: do-then-record.
//   h.do('Set color', () => node.color = next, () => node.color = prev);
//
//   // Gesture (drag): group many frames into one entry via coalesce.
//   h.coalesce((a, b) => a.label === b.label && b.label === 'Move');
//   /* each frame: */ h.record('Move', () => node.pos = p, () => node.pos = p0);
//   /* on drop: */    h.endCoalesce();
//
//   // Transaction: N commands → one atomic entry.
//   h.transaction('Add box', () => {
//       registry.add(box);
//       scene.mark(box);
//   });
//
//   // Snapshot: hard-to-diff state → capture before/after clones.
//   h.snapshot('Rebuild mesh', () => mesh.serialize(), s => mesh.load(s), () => {
//       rebuildMeshInPlace();
//   });
//
//   h.undo(); h.redo();
//   h.canUndo(); h.canRedo();

(function (global) {
    'use strict';

    function now() {
        return (typeof performance !== 'undefined' && performance.now)
            ? performance.now() : Date.now();
    }

    function deepClone(v) {
        if (v == null) return v;
        if (typeof structuredClone === 'function') {
            try { return structuredClone(v); } catch (e) { /* fall through */ }
        }
        return JSON.parse(JSON.stringify(v));
    }

    // Entry shapes (internal):
    //   cmd:   { kind: 'cmd',   label, time, doFn, undoFn, meta }
    //   group: { kind: 'group', label, time, children: [entry,...] }
    //
    // Group children are applied in forward order on redo, reverse order on
    // undo. Groups may nest.

    function History(opts) {
        opts = opts || {};
        this.limit    = opts.limit != null ? opts.limit : 200;
        this._past    = [];
        this._future  = [];
        this._openTx  = [];        // stack of in-progress group entries
        this._coalesceFn = null;   // (prev, next) => bool
        this._marks   = {};        // name → past-length when mark was set
        this._listeners = {};      // event → [fn, ...]
        this._applying = false;    // true while running a doFn/undoFn — blocks
                                   // re-entrant recording (from wrapped
                                   // setters or snapshot appliers).
        if (opts.onChange) this.on('change', opts.onChange);
    }

    History.prototype = {

        // ---- event bus --------------------------------------------------

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
            // Snapshot to a local to avoid mutation-during-iteration hazards
            // when a listener calls off() on itself.
            const snap = arr.slice();
            for (let i = 0; i < snap.length; i++) snap[i](payload, this);
        },

        // ---- core record / do -------------------------------------------

        // Execute `doFn` immediately, then record the pair so it can be
        // undone later. The usual entry point.
        do(label, doFn, undoFn, meta) {
            doFn();
            this.record(label, doFn, undoFn, meta);
        },

        // Record a do/undo pair without executing — for when the caller has
        // already applied the change and just needs the pair captured.
        record(label, doFn, undoFn, meta) {
            if (this._applying) return;   // ignore while replaying
            const entry = {
                kind: 'cmd',
                label: label || '',
                time: now(),
                doFn, undoFn,
                meta: meta || null,
            };
            this._push(entry);
        },

        // Push an entry into the current context (open tx or top-level past).
        // Handles future-clear, coalescing, and limit eviction.
        _push(entry) {
            // Inside a transaction: accumulate in the group's children. The
            // full group is pushed to _past when the outermost end() fires,
            // at which point future is cleared. Until then, redo stack is
            // preserved — an aborted transaction leaves it intact.
            if (this._openTx.length > 0) {
                this._openTx[this._openTx.length - 1].children.push(entry);
                this._emit('record', entry);
                return;
            }

            // Any new top-level entry invalidates the redo stack.
            this._future.length = 0;

            // Coalesce: when an active predicate matches the previous entry,
            // merge so the pair (prev.undoFn, entry.doFn) becomes the kept
            // command. A drag gesture that emits N frames thus collapses to
            // one entry spanning the whole gesture.
            const top = this._past[this._past.length - 1];
            if (this._coalesceFn && top && top.kind === 'cmd'
                && entry.kind === 'cmd' && this._coalesceFn(top, entry)) {
                top.doFn = entry.doFn;
                top.time = entry.time;
                if (entry.label) top.label = entry.label;
                if (entry.meta)  top.meta  = entry.meta;
                this._emit('record', top);
                this._emit('change', this);
                return;
            }

            this._past.push(entry);
            if (this._past.length > this.limit) {
                // Evict oldest — shift any marks pointing into the evicted
                // region down so they still reference valid positions.
                this._past.shift();
                for (const k in this._marks) {
                    if (this._marks[k] > 0) this._marks[k]--;
                    else delete this._marks[k];
                }
            }
            this._emit('record', entry);
            this._emit('change', this);
        },

        // ---- transactions -----------------------------------------------

        begin(label) {
            this._openTx.push({
                kind: 'group',
                label: label || '',
                time: now(),
                children: [],
            });
        },

        end() {
            if (this._openTx.length === 0) {
                throw new Error('History.end: no open transaction');
            }
            const group = this._openTx.pop();
            if (group.children.length === 0) return;   // discard empty
            // Collapse single-child groups to the child itself — keeps the
            // history view clean when a "transaction" only wrapped one cmd.
            const entry = group.children.length === 1 ? group.children[0] : group;
            this._push(entry);
        },

        // Discard the current transaction without recording anything.
        abort() {
            if (this._openTx.length === 0) return;
            this._openTx.pop();
        },

        // Run `fn` inside a transaction. If it throws, the transaction is
        // aborted and the exception re-thrown — nothing is recorded.
        transaction(label, fn) {
            this.begin(label);
            try {
                const r = fn();
                this.end();
                return r;
            } catch (e) {
                this.abort();
                throw e;
            }
        },

        // ---- coalesce ---------------------------------------------------

        // Install a predicate (prev, next) => bool. While active, each new
        // record() that matches the previous entry is merged rather than
        // pushed. Typical gesture:
        //   on drag start: h.coalesce((a, b) => a.label === b.label);
        //   each frame:    h.record('Move', do, undo);
        //   on drag end:   h.endCoalesce();
        coalesce(fn) { this._coalesceFn = fn || null; },
        endCoalesce() { this._coalesceFn = null; },

        // ---- undo / redo ------------------------------------------------

        _applyUndo(entry) {
            if (entry.kind === 'group') {
                for (let i = entry.children.length - 1; i >= 0; i--) {
                    this._applyUndo(entry.children[i]);
                }
            } else {
                entry.undoFn();
            }
        },
        _applyRedo(entry) {
            if (entry.kind === 'group') {
                for (let i = 0; i < entry.children.length; i++) {
                    this._applyRedo(entry.children[i]);
                }
            } else {
                entry.doFn();
            }
        },

        undo() {
            if (this._past.length === 0) return false;
            const entry = this._past.pop();
            this._applying = true;
            try { this._applyUndo(entry); }
            finally { this._applying = false; }
            this._future.push(entry);
            this._emit('undo', entry);
            this._emit('change', this);
            return true;
        },

        redo() {
            if (this._future.length === 0) return false;
            const entry = this._future.pop();
            this._applying = true;
            try { this._applyRedo(entry); }
            finally { this._applying = false; }
            this._past.push(entry);
            this._emit('redo', entry);
            this._emit('change', this);
            return true;
        },

        canUndo() { return this._past.length > 0; },
        canRedo() { return this._future.length > 0; },
        size()    { return this._past.length; },

        clear() {
            this._past.length = 0;
            this._future.length = 0;
            this._openTx.length = 0;
            this._marks = {};
            this._emit('change', this);
        },

        // ---- introspection ----------------------------------------------

        // Shallow metadata for each undo entry — feed a history panel or an
        // edit menu. Ordered oldest → newest.
        entries() {
            const out = new Array(this._past.length);
            for (let i = 0; i < this._past.length; i++) {
                out[i] = this._describe(this._past[i]);
            }
            return out;
        },
        _describe(entry) {
            if (entry.kind === 'group') {
                return {
                    kind: 'group',
                    label: entry.label,
                    time: entry.time,
                    count: entry.children.length,
                };
            }
            return {
                kind: 'cmd',
                label: entry.label,
                time: entry.time,
                meta: entry.meta,
            };
        },

        // ---- marks ------------------------------------------------------

        // Tag the current stack depth with a name. Later, rewindTo(name)
        // undoes until the stack is back at that depth. Useful for
        // "revert since save" or branch points.
        mark(name) {
            this._marks[name] = this._past.length;
        },

        // Undo until past depth matches the mark. Returns the number of
        // entries undone, or false if the mark is unknown / already past.
        rewindTo(name) {
            const target = this._marks[name];
            if (target == null) return false;
            if (target > this._past.length) return 0;
            let count = 0;
            while (this._past.length > target) {
                if (!this.undo()) break;
                count++;
            }
            return count;
        },

        // ---- conveniences -----------------------------------------------

        // Auto-record assignments to `obj[prop]`. Reads pass through; writes
        // record a record() entry. Returns an unwrap function that restores
        // the property to a plain data field at its current value.
        //
        // Assignments performed internally (via the recorded undoFn/doFn
        // closures) mutate a closed-over slot directly, so they don't
        // re-trigger the setter — no re-entrant recording to worry about.
        wrap(obj, prop, label) {
            const self = this;
            let value = obj[prop];
            Object.defineProperty(obj, prop, {
                configurable: true,
                enumerable: true,
                get() { return value; },
                set(next) {
                    if (self._applying) { value = next; return; }
                    const prev = value;
                    value = next;
                    self.record(label || prop,
                        () => { value = next; },
                        () => { value = prev; });
                },
            });
            return function unwrap() {
                Object.defineProperty(obj, prop, {
                    configurable: true, enumerable: true, writable: true, value,
                });
            };
        },

        // Capture a before-snapshot, run `mutation()`, capture an after-
        // snapshot, and record a cmd whose do/undo swap between the two
        // via `setState`. Snapshots are deep-cloned so later mutations to
        // the live state don't alias into history. Best for hard-to-diff
        // operations; avoid for very large state.
        snapshot(label, getState, setState, mutation) {
            const before = deepClone(getState());
            const r = mutation();
            const after = deepClone(getState());
            this.record(label,
                () => setState(deepClone(after)),
                () => setState(deepClone(before)));
            return r;
        },
    };

    global.History = History;
})(typeof window !== 'undefined' ? window : globalThis);
