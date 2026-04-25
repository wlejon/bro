// screens.js — overlay state, backed by lib/screens.js.
//
// Preserves the existing P.Screens API (menuUp/Down/Select/listItems/
// updateMenu/menuIndex, plus DOM helpers) so app.js stays untouched.
var P = P || {};

P.Screens = (function () {
    'use strict';

    var S = Screens.create({
        overlay:      '#overlay',
        onMenuMove:   function () { P.Audio.sfxMenu(); },
        onMenuSelect: function () { P.Audio.sfxMenu(); },
    });

    // app.js sets this; lib invokes it on mouse-click (which sends keydown
    // 'Enter' to the active screen).
    var confirmHandler = null;

    function defineSimple(name) {
        S.define(name, {
            enter: function () { S.showOverlay(name); S.updateSelection(name); },
            keydown: function (key) {
                if ((key === 'Enter' || key === ' ') && confirmHandler) {
                    var items = S.getMenuItems(name);
                    var item = items[menuIdx];
                    confirmHandler(item ? item.getAttribute('data-action') : null);
                }
            },
        });
    }
    defineSimple('title');
    defineSimple('paused');
    defineSimple('gameover');

    var menuIdx = 0;

    return {
        // current screen name; legacy default is "title"
        current: 'title',
        // app.js assigns to this; lib triggers it on click.
        set onConfirm(fn) { confirmHandler = fn; },
        get onConfirm()   { return confirmHandler; },
        // legacy mutable index — read by app.js mouse handler
        get menuIndex() { return menuIdx; },
        set menuIndex(i) { menuIdx = i; S.setMenuIndex(i); },

        listItems: function () { return S.getMenuItems(this.current); },

        switchTo: function (name) {
            this.current = name;
            menuIdx = 0;
            S.switchTo(name);
        },
        hideOverlay: function () { this.current = ''; S.hideOverlay(); },

        updateMenu: function () { S.setMenuIndex(menuIdx); },

        menuUp: function () {
            var items = this.listItems();
            if (!items.length) return;
            menuIdx = (menuIdx - 1 + items.length) % items.length;
            S.setMenuIndex(menuIdx);
            P.Audio.sfxMenu();
        },
        menuDown: function () {
            var items = this.listItems();
            if (!items.length) return;
            menuIdx = (menuIdx + 1) % items.length;
            S.setMenuIndex(menuIdx);
            P.Audio.sfxMenu();
        },
        menuSelect: function () {
            var items = this.listItems();
            if (!items.length) return null;
            var item = items[menuIdx];
            return item ? item.getAttribute('data-action') : null;
        },

        // DOM helpers — text setters into specific overlay elements
        setGameOverStats: function (score, isNew) {
            var se = document.getElementById('go-score');
            var he = document.getElementById('go-high');
            var ne = document.getElementById('go-new');
            if (se) se.textContent = String(score);
            if (he) he.textContent = String(P.Storage.highScore);
            if (ne) ne.style.display = isNew ? 'block' : 'none';
        },
        setTitleHigh: function () {
            var el = document.getElementById('title-high');
            if (el) el.textContent = String(P.Storage.highScore);
        },
        setGameOverTitle: function (text) {
            var el = document.getElementById('gameover-title');
            if (el) el.textContent = text;
        },
    };
})();
