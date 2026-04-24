// screens.js — overlay screens wiring for Pegbounce.
// Layer between apps/lib/screens.js and app.js. Handles UI DOM rendering
// for level select, guide select, high scores, settings; delegates
// menu key navigation to the library.

'use strict';
(function (global) {

    // Build level select grid from store state.
    function renderLevelGrid(gridEl, LEVELS, store, onClick) {
        gridEl.innerHTML = '';
        const unlocked = store.get('unlocked') || 1;
        const bestScores = store.get('best')   || {};
        const starMap    = store.get('stars')  || {};
        for (let i = 0; i < LEVELS.length; i++) {
            const lv = LEVELS[i];
            const locked = i >= unlocked;
            const best = bestScores[lv.id];
            const stars = starMap[lv.id] || 0;
            const starStr = ['★','★','★'].slice(0,stars).join('')
                         + ['☆','☆','☆'].slice(0,3-stars).join('');
            const tile = document.createElement('div');
            tile.className = 'level-tile' + (locked ? ' locked' : '');
            tile.dataset.idx = i;
            tile.innerHTML =
                '<div class="lt-num">' + (i + 1) + '</div>' +
                '<div class="lt-name">' + lv.name + '</div>' +
                '<div class="lt-stars">' + starStr + '</div>' +
                '<div class="lt-best">' + (best ? ('Best ' + best) : (locked ? 'Locked' : 'New')) + '</div>';
            if (!locked) tile.addEventListener('click', () => onClick(i));
            gridEl.appendChild(tile);
        }
    }

    function renderGuideCards(rootEl, GUIDES, selectedId, onSelect) {
        rootEl.innerHTML = '';
        for (const g of GUIDES) {
            const card = document.createElement('div');
            card.className = 'guide-card' + (g.id === selectedId ? ' selected' : '');
            card.dataset.id = g.id;
            card.innerHTML =
                '<div class="gc-icon" style="color:' + g.color + '">' + g.icon + '</div>' +
                '<div class="gc-name">' + g.name + '</div>' +
                '<div class="gc-blurb">' + g.blurb + '</div>';
            card.addEventListener('click', () => onSelect(g.id));
            rootEl.appendChild(card);
        }
    }

    function renderHighScores(listEl, LEVELS, store) {
        const bestMap = store.get('best')  || {};
        const starMap = store.get('stars') || {};
        if (!Object.keys(bestMap).length) {
            listEl.textContent = 'No scores yet. Clear a level to post a score.';
            return;
        }
        const lines = [];
        for (let i = 0; i < LEVELS.length; i++) {
            const lv = LEVELS[i];
            const best = bestMap[lv.id];
            if (best == null) continue;
            const stars = starMap[lv.id] || 0;
            lines.push('L' + (i+1).toString().padStart(2,' ') + '  ' +
                       lv.name.padEnd(16, ' ') + '  ' +
                       String(best).padStart(7, ' ') + '   ' +
                       ['★','★','★'].slice(0, stars).join('') +
                       ['☆','☆','☆'].slice(0, 3-stars).join(''));
        }
        listEl.textContent = lines.join('\n');
    }

    function fmtSettings(value, kind) {
        if (kind === 'bool') return value ? 'ON' : 'OFF';
        if (kind === 'pct')  return Math.round(value * 100);
        return value;
    }

    function renderSettings(store) {
        const bind = (id, k, kind) => {
            const el = document.getElementById('opt-' + id);
            if (el) el.textContent = fmtSettings(store.get(k), kind);
        };
        bind('sfxVol', 'sfxVol', 'pct');
        bind('musicVol', 'musicVol', 'pct');
        bind('trajectory', 'trajectory', 'bool');
        bind('screenshake', 'screenshake', 'bool');
    }

    global.PegScreens = {
        renderLevelGrid, renderGuideCards, renderHighScores, renderSettings,
    };

})(typeof window !== 'undefined' ? window : globalThis);
