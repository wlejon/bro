// screens.js — Screen manager for title, playing, pause, landed, game over
var T = T || {};

T.Screens = (function() {
    var currentName = "";
    var menuIndex = 0;
    var overlay = null;
    var hud = null;
    var telemetry = null;

    // Background starfield for title screen
    var bgStars = [];
    var bgLanders = [];

    function initBg(W, H) {
        bgStars = [];
        for (var i = 0; i < 120; i++) {
            bgStars.push({
                x: Math.random() * W,
                y: Math.random() * H,
                b: 0.15 + Math.random() * 0.7,
                drift: 0.005 + Math.random() * 0.02
            });
        }
        bgLanders = [];
        for (var j = 0; j < 3; j++) {
            bgLanders.push({
                x: Math.random() * W,
                y: 80 + Math.random() * (H * 0.5),
                vx: (Math.random() - 0.5) * 0.05,
                vy: 0.02 + Math.random() * 0.03,
                ang: (Math.random() - 0.5) * 0.4,
                alpha: 0.12 + Math.random() * 0.15
            });
        }
    }

    function updateBg(dt, W, H) {
        for (var i = 0; i < bgStars.length; i++) {
            var s = bgStars[i];
            s.y += s.drift * dt;
            if (s.y > H) { s.y = 0; s.x = Math.random() * W; }
        }
        for (var j = 0; j < bgLanders.length; j++) {
            var L = bgLanders[j];
            L.x += L.vx * dt;
            L.y += L.vy * dt;
            if (L.y > H + 20) { L.y = -20; L.x = Math.random() * W; }
            if (L.x < -20) L.x = W + 20;
            if (L.x > W + 20) L.x = -20;
        }
    }

    function drawBg(ctx, W, H) {
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, W, H);
        for (var i = 0; i < bgStars.length; i++) {
            var s = bgStars[i];
            ctx.globalAlpha = s.b;
            ctx.fillStyle = "#ffffff";
            ctx.fillRect(s.x, s.y, 1, 1);
        }
        ctx.globalAlpha = 1;

        // Ghost landers drifting
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1;
        for (var j = 0; j < bgLanders.length; j++) {
            var L = bgLanders[j];
            ctx.globalAlpha = L.alpha;
            ctx.save();
            ctx.translate(L.x, L.y);
            ctx.rotate(L.ang);
            ctx.beginPath();
            ctx.moveTo(0, -10);
            ctx.lineTo(-7, -2);
            ctx.lineTo(-7, 5);
            ctx.lineTo(7, 5);
            ctx.lineTo(7, -2);
            ctx.closePath();
            ctx.stroke();
            ctx.restore();
        }
        ctx.globalAlpha = 1;
    }

    function showOverlay(screenId) {
        if (!overlay) overlay = document.getElementById("overlay");
        var divs = overlay.children;
        for (var i = 0; i < divs.length; i++) divs[i].style.display = "none";
        var el = document.getElementById("screen-" + screenId);
        if (el) el.style.display = "block";
        overlay.style.display = "block";
    }

    function hideOverlay() {
        if (!overlay) overlay = document.getElementById("overlay");
        overlay.style.display = "none";
    }

    function showHud(show) {
        if (!hud) hud = document.getElementById("hud");
        if (!telemetry) telemetry = document.getElementById("telemetry");
        hud.style.display = show ? "flex" : "none";
        telemetry.style.display = show ? "flex" : "none";
    }

    function getMenuItems(screenId) {
        var el = document.getElementById("screen-" + screenId);
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

    function updateSelection(screenId) {
        var items = getMenuItems(screenId);
        for (var i = 0; i < items.length; i++) {
            items[i].className = (i === menuIndex) ? "menu-item selected" : "menu-item";
        }
    }

    function menuNav(screenId, key, onSelect) {
        var items = getMenuItems(screenId);
        if (!items.length) return;
        if (key === "ArrowUp") {
            menuIndex = (menuIndex - 1 + items.length) % items.length;
            updateSelection(screenId);
            T.Audio.sfxMenuMove();
        } else if (key === "ArrowDown") {
            menuIndex = (menuIndex + 1) % items.length;
            updateSelection(screenId);
            T.Audio.sfxMenuMove();
        } else if (key === "Enter") {
            T.Audio.sfxMenuSelect();
            if (onSelect) onSelect(items[menuIndex]);
        }
    }

    function updateHud() {
        var s = T.Game.getState();
        if (!s) return;
        document.getElementById("hud-score").textContent = String(s.score);
        document.getElementById("hud-hi").textContent = String(T.Storage.highScore);
        document.getElementById("hud-level").textContent = String(s.level);
        document.getElementById("hud-landed").textContent = String(s.landings);
        document.getElementById("hud-fuel").textContent = String(Math.max(0, Math.round(s.lander.fuel)));

        var L = s.lander;
        var groundY = s.H * 0.78; // rough baseline for altitude display
        // Better: use actual terrain Y at lander x. Inline linear search:
        var terrain = s.terrain;
        var tx = L.x;
        if (tx < terrain.points[0].x) tx = terrain.points[0].x;
        if (tx > terrain.points[terrain.points.length - 1].x) tx = terrain.points[terrain.points.length - 1].x;
        for (var i = 1; i < terrain.points.length; i++) {
            if (terrain.points[i].x >= tx) {
                var a = terrain.points[i - 1], b = terrain.points[i];
                var tt = (tx - a.x) / (b.x - a.x || 1);
                groundY = a.y + (b.y - a.y) * tt;
                break;
            }
        }
        var alt = Math.max(0, Math.round(groundY - L.y));
        document.getElementById("tel-alt").textContent = String(alt);
        document.getElementById("tel-hvel").textContent = L.vx.toFixed(2);
        document.getElementById("tel-vvel").textContent = L.vy.toFixed(2);
    }

    function switchTo(name, W, H) {
        currentName = name;
        menuIndex = 0;
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
            T.Game.start(W || 900, H || 800);
            updateHud();
        } else if (name === "paused") {
            showOverlay("pause");
            updateSelection("pause");
            T.Game.setPaused(true);
        } else if (name === "landed") {
            var s = T.Game.getState();
            var lines = [];
            lines.push("LEVEL        " + s.level);
            lines.push("PAD WIDTH    " + s.lastLandingPadWidth);
            lines.push("BONUS        +" + s.lastLandingBonus);
            lines.push("SCORE        " + s.score);
            lines.push("FUEL LEFT    " + Math.round(s.lander.fuel));
            document.getElementById("landed-stats").textContent = lines.join("\n");
            showOverlay("landed");
            updateSelection("landed");
        } else if (name === "gameover") {
            var s2 = T.Game.getState();
            var isHi = T.Storage.maybeUpdate(s2 ? s2.score : 0);
            var lines2 = [];
            lines2.push("SCORE     " + (s2 ? s2.score : 0));
            lines2.push("LEVEL     " + (s2 ? s2.level : 1));
            lines2.push("LANDED    " + (s2 ? s2.landings : 0));
            lines2.push("HI        " + T.Storage.highScore);
            if (isHi) { lines2.push(""); lines2.push("NEW HIGH SCORE!"); }
            document.getElementById("gameover-stats").textContent = lines2.join("\n");
            document.getElementById("gameover-title").textContent = "CRASHED";
            showOverlay("gameover");
            showHud(false);
            updateSelection("gameover");
        }
    }

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
                return;
            }
        } else if (currentName === "paused") {
            if (key === "Escape") {
                T.Game.setPaused(false);
                hideOverlay();
                showHud(true);
                currentName = "playing";
                return;
            }
            menuNav("pause", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "resume") {
                    T.Game.setPaused(false);
                    hideOverlay();
                    showHud(true);
                    currentName = "playing";
                } else if (act === "restart") {
                    switchTo("playing", W, H);
                } else if (act === "quit") {
                    switchTo("title");
                }
            });
        } else if (currentName === "landed") {
            menuNav("landed", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "next") {
                    T.Game.advanceLevel();
                    hideOverlay();
                    showHud(true);
                    currentName = "playing";
                }
            });
        } else if (currentName === "gameover") {
            menuNav("gameover", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "restart") switchTo("playing", W, H);
                else if (act === "quit") switchTo("title");
            });
        }
    }

    function keyup(key) { /* handled by lib/input sampling */ }

    function update(dt, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            updateBg(dt, W, H);
        } else if (currentName === "playing") {
            T.Game.update(dt, W, H);
            if (T.Game.isLanded()) {
                switchTo("landed");
                return;
            }
            if (T.Game.isGameOver()) {
                switchTo("gameover");
                return;
            }
            updateHud();
        } else if (currentName === "landed") {
            // Keep world drawn beneath but frozen; no game sim update.
        }
    }

    function draw(ctx, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            drawBg(ctx, W, H);
        } else {
            ctx.fillStyle = "#000000";
            ctx.fillRect(0, 0, W, H);
            T.Game.draw(ctx, W, H);
        }
    }

    function init(W, H) {
        initBg(W || 900, H || 800);
    }

    return {
        init: init,
        switchTo: switchTo,
        keydown: keydown,
        keyup: keyup,
        update: update,
        draw: draw,
        getName: function() { return currentName; }
    };
})();
