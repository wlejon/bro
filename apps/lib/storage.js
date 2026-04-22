// storage.js — namespaced localStorage wrapper + high-score tables.
//
// Usage:
//   const S = Storage.create("crater");
//   S.load({ name: "Player", sfxVol: 0.8 });   // returns merged defaults
//   S.set("name", "Jonny");
//   S.save();
//   const hs = Storage.highscores("crater", 10);
//   hs.add({ score: 120, name: "JB", date: "2026-04-22" });

(function (global) {
    'use strict';

    function safeGet(key) {
        try { return localStorage.getItem(key); } catch (e) { return null; }
    }
    function safeSet(key, value) {
        try { localStorage.setItem(key, value); } catch (e) {}
    }

    function create(namespace) {
        const key = namespace + ':settings';
        let data = {};
        return {
            load(defaults) {
                data = Object.assign({}, defaults || {});
                const raw = safeGet(key);
                if (raw) {
                    try {
                        const parsed = JSON.parse(raw);
                        for (const k in parsed) {
                            if (Object.prototype.hasOwnProperty.call(data, k)) {
                                data[k] = parsed[k];
                            }
                        }
                    } catch (e) {}
                }
                return data;
            },
            save() { safeSet(key, JSON.stringify(data)); },
            get(k)      { return data[k]; },
            set(k, v)   { data[k] = v; },
            all()       { return data; },
        };
    }

    // High-score table: sorted descending by `score` (or ascending when
    // `ascending: true` — for timed modes where lower is better).
    function highscores(namespace, maxEntries, opts) {
        opts = opts || {};
        const key = namespace + ':highscores';
        const max = maxEntries || 10;
        const field = opts.field || 'score';
        const asc   = !!opts.ascending;
        const cmp   = asc
            ? (a, b) => a[field] - b[field]
            : (a, b) => b[field] - a[field];

        function loadAll() {
            const raw = safeGet(key);
            if (!raw) return [];
            try { return JSON.parse(raw) || []; } catch (e) { return []; }
        }
        function saveAll(list) { safeSet(key, JSON.stringify(list)); }

        return {
            list: loadAll,
            add(entry) {
                const list = loadAll();
                list.push(entry);
                list.sort(cmp);
                list.length = Math.min(list.length, max);
                saveAll(list);
                return list;
            },
            qualifies(value) {
                const list = loadAll();
                if (list.length < max) return true;
                return asc ? (value < list[list.length - 1][field])
                           : (value > list[list.length - 1][field]);
            },
            clear() { saveAll([]); },
        };
    }

    global.Storage = { create, highscores };
})(typeof window !== 'undefined' ? window : globalThis);
