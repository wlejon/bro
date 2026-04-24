// dictionary.js — loads words.txt and builds fast lookup + prefix trie.
'use strict';
var W = window.W = window.W || {};

W.Dictionary = (function () {
    var words = null;          // Set<string>
    var prefixes = null;       // Set<string> of all prefixes (for branch pruning)
    var loaded = false;
    var loadError = null;

    // Simple prefix set (hash-based). ~0.5-1M entries for a 30k-word list.
    function buildPrefixes(set) {
        var p = new Set();
        set.forEach(function (w) {
            for (var i = 1; i <= w.length; i++) {
                p.add(w.substring(0, i));
            }
        });
        return p;
    }

    function load() {
        // fetch('words.txt') relative to the app root.
        return fetch('words.txt').then(function (r) {
            if (!r.ok) throw new Error('words.txt fetch failed: ' + r.status);
            return r.text();
        }).then(function (txt) {
            var lines = txt.split(/\r?\n/);
            var set = new Set();
            for (var i = 0; i < lines.length; i++) {
                var w = lines[i].trim().toLowerCase();
                if (w.length >= 3 && w.length <= 16 && /^[a-z]+$/.test(w)) {
                    set.add(w);
                }
            }
            words = set;
            prefixes = buildPrefixes(set);
            loaded = true;
            return set.size;
        }).catch(function (err) {
            loadError = err;
            loaded = false;
            throw err;
        });
    }

    function isWord(s) {
        if (!loaded || !s) return false;
        return words.has(String(s).toLowerCase());
    }

    function isPrefix(s) {
        if (!loaded || !s) return false;
        return prefixes.has(String(s).toLowerCase());
    }

    function count() { return words ? words.size : 0; }

    // Scan the dictionary for all words that are "findable" (match the letters
    // present in a list). Not used in-game by default but useful for puzzle
    // generation: return words constructible from `letterMultiset` (a
    // { a: n, b: n, ... } map) with optional lengthMin/Max.
    function findConstructible(letterMultiset, lengthMin, lengthMax, limit) {
        if (!loaded) return [];
        lengthMin = lengthMin || 3;
        lengthMax = lengthMax || 12;
        limit     = limit     || 200;
        var out = [];
        words.forEach(function (w) {
            if (out.length >= limit) return;
            if (w.length < lengthMin || w.length > lengthMax) return;
            var m = Object.assign({}, letterMultiset);
            var ok = true;
            for (var i = 0; i < w.length; i++) {
                var ch = w.charAt(i);
                if (!m[ch] || m[ch] <= 0) { ok = false; break; }
                m[ch]--;
            }
            if (ok) out.push(w);
        });
        return out;
    }

    // Testing helper — force-load a set from an array of words.
    function _setWords(list) {
        words = new Set(list.map(function (s) { return s.toLowerCase(); }));
        prefixes = buildPrefixes(words);
        loaded = true;
        loadError = null;
    }

    return {
        load: load,
        isWord: isWord,
        isPrefix: isPrefix,
        count: count,
        findConstructible: findConstructible,
        loaded: function () { return loaded; },
        error: function () { return loadError; },
        _setWords: _setWords
    };
})();
