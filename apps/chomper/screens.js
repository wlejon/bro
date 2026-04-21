// screens.js — screen/state management
var P = P || {};

P.Screens = {
    current: "title",
    menuIndex: 0,

    getScreen: function() { return document.getElementById("screen-" + this.current); },

    listItems: function() {
        var sc = this.getScreen();
        if (!sc) return [];
        return sc.querySelectorAll(".menu-item");
    },

    switchTo: function(name) {
        var prev = document.getElementById("screen-" + this.current);
        if (prev) prev.style.display = "none";
        this.current = name;
        var next = document.getElementById("screen-" + name);
        if (next) {
            next.style.display = "block";
            this.menuIndex = 0;
            this.updateMenu();
        }
        var overlay = document.getElementById("overlay");
        overlay.style.display = name ? "block" : "none";
    },

    hideOverlay: function() {
        var overlay = document.getElementById("overlay");
        overlay.style.display = "none";
        var prev = document.getElementById("screen-" + this.current);
        if (prev) prev.style.display = "none";
        this.current = "";
    },

    updateMenu: function() {
        var items = this.listItems();
        for (var i = 0; i < items.length; i++) {
            if (i === this.menuIndex) items[i].classList.add("selected");
            else items[i].classList.remove("selected");
        }
    },

    menuUp: function() {
        var items = this.listItems();
        if (!items.length) return;
        this.menuIndex = (this.menuIndex - 1 + items.length) % items.length;
        this.updateMenu();
        P.Audio.sfxMenu();
    },

    menuDown: function() {
        var items = this.listItems();
        if (!items.length) return;
        this.menuIndex = (this.menuIndex + 1) % items.length;
        this.updateMenu();
        P.Audio.sfxMenu();
    },

    menuSelect: function() {
        var items = this.listItems();
        if (!items.length) return null;
        var item = items[this.menuIndex];
        return item ? item.getAttribute("data-action") : null;
    },

    setGameOverStats: function(score, isNew) {
        var se = document.getElementById("go-score");
        var he = document.getElementById("go-high");
        var ne = document.getElementById("go-new");
        if (se) se.textContent = String(score);
        if (he) he.textContent = String(P.Storage.highScore);
        if (ne) ne.style.display = isNew ? "block" : "none";
    },

    setTitleHigh: function() {
        var el = document.getElementById("title-high");
        if (el) el.textContent = String(P.Storage.highScore);
    },

    setGameOverTitle: function(text) {
        var el = document.getElementById("gameover-title");
        if (el) el.textContent = text;
    }
};
