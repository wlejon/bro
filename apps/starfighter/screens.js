// screens.js — Overlay state machine for title/briefing/playing/pause/gameover/victory.
var N = N || {};

N.Screens = (function() {
    "use strict";

    var currentName = "";
    var menuIndex = 0;
    var overlay = null;
    var hud = null;
    var mouseAttached = false;
    var lastW = 1024, lastH = 768;

    // --- Title backdrop: slow-moving star tunnel for atmosphere -----------
    var titleStars = [];
    var TITLE_STAR_COUNT = 200;

    function initBg(W, H) {
        titleStars.length = 0;
        for (var i = 0; i < TITLE_STAR_COUNT; i++) {
            titleStars.push({
                x: (Math.random() * 2 - 1),
                y: (Math.random() * 2 - 1),
                z: 0.2 + Math.random() * 0.8,
                s: 0.3 + Math.random() * 0.9
            });
        }
    }

    function updateBg(dt) {
        var adv = 0.00015 * dt;
        for (var i = 0; i < titleStars.length; i++) {
            var s = titleStars[i];
            s.z -= adv;
            if (s.z <= 0.05) {
                s.x = (Math.random() * 2 - 1);
                s.y = (Math.random() * 2 - 1);
                s.z = 1.0;
                s.s = 0.3 + Math.random() * 0.9;
            }
        }
    }

    function drawBg(ctx, W, H) {
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, W, H);
        ctx.fillStyle = "#fff";
        for (var i = 0; i < titleStars.length; i++) {
            var s = titleStars[i];
            var scale = 1 / s.z;
            var px = W * 0.5 + s.x * W * 0.5 * scale;
            var py = H * 0.5 + s.y * H * 0.5 * scale;
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            ctx.globalAlpha = Math.min(1, (1 - s.z) * 1.4);
            var sz = s.s * (2 - s.z);
            ctx.fillRect(px | 0, py | 0, Math.max(1, sz | 0), Math.max(1, sz | 0));
        }
        ctx.globalAlpha = 1;
    }

    // --- Overlay DOM helpers ----------------------------------------------
    function showOverlay(id) {
        if (!overlay) overlay = document.getElementById("overlay");
        var divs = overlay.children;
        for (var i = 0; i < divs.length; i++) divs[i].style.display = "none";
        var el = document.getElementById("screen-" + id);
        if (el) el.style.display = "block";
        overlay.style.display = "flex";
    }
    function hideOverlay() {
        if (!overlay) overlay = document.getElementById("overlay");
        overlay.style.display = "none";
    }
    function showHud(show) {
        if (!hud) hud = document.getElementById("hud");
        hud.style.display = show ? "block" : "none";
    }

    function getMenuItems(id) {
        var el = document.getElementById("screen-" + id);
        if (!el) return [];
        var items = [];
        var containers = el.querySelectorAll(".menu-items");
        for (var ci = 0; ci < containers.length; ci++) {
            var children = containers[ci].children;
            for (var i = 0; i < children.length; i++) {
                if (children[i].className.indexOf("menu-item") !== -1) items.push(children[i]);
            }
        }
        return items;
    }
    function updateSelection(id) {
        var items = getMenuItems(id);
        for (var i = 0; i < items.length; i++) {
            items[i].className = (i === menuIndex) ? "menu-item selected" : "menu-item";
        }
    }

    function activeScreenId() {
        if (currentName === "paused")  return "pause";
        if (currentName === "playing") return "";
        return currentName;
    }

    function findMenuItem(target, ov) {
        while (target && target !== ov) {
            if (target.className && target.className.indexOf("menu-item") !== -1 &&
                target.className.indexOf("menu-items") === -1) {
                return target;
            }
            target = target.parentNode;
        }
        return null;
    }

    function ensureMouse() {
        if (mouseAttached) return;
        if (!overlay) overlay = document.getElementById("overlay");
        if (!overlay) return;
        mouseAttached = true;

        // Attach listeners directly to every menu item rather than
        // delegating through the overlay. Event target propagation inside
        // nested flex containers has bitten us before, and direct binding
        // is simpler anyway.
        var screens = overlay.querySelectorAll(".screen");
        for (var si = 0; si < screens.length; si++) {
            var sc = screens[si];
            var scId = sc.id.replace(/^screen-/, "");
            var items = sc.querySelectorAll(".menu-item");
            for (var ii = 0; ii < items.length; ii++) {
                bindMenuItem(items[ii], scId, ii);
            }
        }
    }

    function bindMenuItem(item, sid, index) {
        item.addEventListener("mouseenter", function() {
            if (activeScreenId() !== sid) return;
            if (menuIndex !== index) {
                menuIndex = index;
                updateSelection(sid);
                N.Audio.sfxMenuMove();
            }
        });
        item.addEventListener("click", function() {
            if (activeScreenId() !== sid) return;
            menuIndex = index;
            updateSelection(sid);
            keydown("Enter", lastW, lastH);
        });
    }

    function menuNav(id, key, onSelect) {
        var items = getMenuItems(id);
        if (!items.length) return;
        if (key === "ArrowUp") {
            menuIndex = (menuIndex - 1 + items.length) % items.length;
            updateSelection(id);
            N.Audio.sfxMenuMove();
        } else if (key === "ArrowDown") {
            menuIndex = (menuIndex + 1) % items.length;
            updateSelection(id);
            N.Audio.sfxMenuMove();
        } else if (key === "Enter") {
            N.Audio.sfxMenuSelect();
            if (onSelect) onSelect(items[menuIndex]);
        }
    }

    // --- HUD binding -------------------------------------------------------
    function updateHud() {
        var s = N.Game.getState();
        if (!s) return;
        document.getElementById("hud-score").textContent   = String(s.score);
        document.getElementById("hud-hi").textContent      = String(N.Storage.highScore);
        document.getElementById("hud-wave").textContent    = s.waveLabel;
        document.getElementById("hud-shields").textContent = s.shieldBar;

        var lock = document.getElementById("hud-lock");
        if (s.lockActive) {
            lock.textContent = "— LOCK —";
            lock.classList.add("active");
        } else {
            lock.classList.remove("active");
        }

        var radio = document.getElementById("hud-radio");
        if (s.radio) {
            radio.textContent = s.radio;
            radio.classList.add("active");
        } else {
            radio.classList.remove("active");
        }
    }

    function showVictory() {
        var s = N.Game.getState();
        var stats = "CITADEL CAMPAIGN " + (s.loop) + " COMPLETE\n\n" +
                    "SCORE   " + s.score;
        document.getElementById("victory-stats").textContent = stats;
        showOverlay("victory");
        showHud(false);
        menuIndex = 0;
        updateSelection("victory");
    }

    // --- Switching ---------------------------------------------------------
    function switchTo(name, W, H) {
        currentName = name;
        menuIndex = 0;
        if (W) lastW = W;
        if (H) lastH = H;
        ensureMouse();
        // Cursor: visible on all overlay states, hidden during playing.
        if (name === "playing") document.body.classList.add("playing");
        else document.body.classList.remove("playing");
        if (name === "title") {
            showOverlay("title");
            showHud(false);
            updateSelection("title");
        } else if (name === "howtoplay") {
            showOverlay("howtoplay");
            updateSelection("howtoplay");
        } else if (name === "playing") {
            hideOverlay();
            showHud(true);
            N.Game.start(W || 1024, H || 768);
            updateHud();
        } else if (name === "paused") {
            showOverlay("pause");
            updateSelection("pause");
            N.Game.setPaused(true);
        } else if (name === "gameover") {
            var s = N.Game.getState();
            var isHi = N.Storage.maybeUpdate(s ? s.score : 0);
            var lines = [];
            lines.push("SCORE    " + (s ? s.score : 0));
            lines.push("SECTOR   " + (s ? s.waveLabel : "1-1"));
            lines.push("HI       " + N.Storage.highScore);
            if (isHi) { lines.push(""); lines.push("NEW HIGH SCORE!"); }
            document.getElementById("gameover-stats").textContent = lines.join("\n");
            showOverlay("gameover");
            showHud(false);
            updateSelection("gameover");
        } else if (name === "victory") {
            showVictory();
        }
    }

    // --- Key routing -------------------------------------------------------
    function keydown(key, W, H) {
        if (currentName === "title") {
            menuNav("title", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "play") switchTo("playing", W, H);
                else if (act === "howtoplay") switchTo("howtoplay");
                else if (act === "quit") { try { window.close && window.close(); } catch(e) {} }
            });
        } else if (currentName === "howtoplay") {
            if (key === "Escape") { switchTo("title"); return; }
            menuNav("howtoplay", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "back") switchTo("title");
            });
        } else if (currentName === "playing") {
            if (key === "Escape" || key === "p" || key === "P") {
                switchTo("paused");
            } else if (key === "t" || key === "T") {
                N.Game.toggleTargetingComputer();
            }
        } else if (currentName === "paused") {
            if (key === "Escape") {
                N.Game.setPaused(false);
                hideOverlay();
                showHud(true);
                currentName = "playing";
                document.body.classList.add("playing");
                return;
            }
            menuNav("pause", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "resume") {
                    N.Game.setPaused(false);
                    hideOverlay();
                    showHud(true);
                    currentName = "playing";
                    document.body.classList.add("playing");
                } else if (act === "restart") {
                    switchTo("playing", W, H);
                } else if (act === "quit") {
                    switchTo("title");
                }
            });
        } else if (currentName === "gameover") {
            menuNav("gameover", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "restart") switchTo("playing", W, H);
                else if (act === "quit") switchTo("title");
            });
        } else if (currentName === "victory") {
            menuNav("victory", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "continue") {
                    N.Game.advanceLoop();
                    hideOverlay();
                    showHud(true);
                    currentName = "playing";
                    document.body.classList.add("playing");
                }
            });
        }
    }

    // --- Per-frame update / draw -------------------------------------------
    function update(dt, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            updateBg(dt);
        } else if (currentName === "playing") {
            N.Game.update(dt, W, H);
            if (N.Game.isGameOver()) { switchTo("gameover"); return; }
            var s = N.Game.getState();
            if (s && s.victoryPending) { switchTo("victory"); return; }
            updateHud();
        }
    }

    function draw(ctx, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            drawBg(ctx, W, H);
        } else if (currentName === "playing" || currentName === "paused" ||
                   currentName === "gameover" || currentName === "victory") {
            N.Game.draw(ctx, W, H);
        }
    }

    function init(W, H) { initBg(W, H); }

    return {
        init: init,
        switchTo: switchTo,
        keydown: keydown,
        update: update,
        draw: draw,
        getName: function() { return currentName; }
    };
})();
