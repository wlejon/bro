// screens.js — Screen manager for title, playing, pause, game over, how-to
var A = A || {};

A.Screens = (function() {
    var currentName = "";
    var menuIndex = 0;
    var overlay = null;
    var hud = null;

    // Background asteroids drifting behind the title
    var bgAsteroids = [];

    function initBg(W, H) {
        bgAsteroids = [];
        for (var i = 0; i < 8; i++) {
            var radius = 18 + Math.random() * 30;
            var a = Math.random() * Math.PI * 2;
            var sp = 0.02 + Math.random() * 0.04;
            bgAsteroids.push({
                x: Math.random() * W,
                y: Math.random() * H,
                vx: Math.cos(a) * sp,
                vy: Math.sin(a) * sp,
                rot: Math.random() * Math.PI * 2,
                rotSpeed: (Math.random() - 0.5) * 0.0008,
                radius: radius,
                shape: makeShape(radius),
                alpha: 0.15 + Math.random() * 0.15
            });
        }
    }

    function makeShape(radius) {
        var pts = [];
        var n = 10 + Math.floor(Math.random() * 4);
        for (var i = 0; i < n; i++) {
            var a = (i / n) * Math.PI * 2;
            var r = radius * (0.75 + Math.random() * 0.45);
            pts.push({ x: Math.cos(a) * r, y: Math.sin(a) * r });
        }
        return pts;
    }

    function updateBg(dt, W, H) {
        for (var i = 0; i < bgAsteroids.length; i++) {
            var a = bgAsteroids[i];
            a.x += a.vx * dt;
            a.y += a.vy * dt;
            a.rot += a.rotSpeed * dt;
            if (a.x < -a.radius) a.x = W + a.radius;
            else if (a.x > W + a.radius) a.x = -a.radius;
            if (a.y < -a.radius) a.y = H + a.radius;
            else if (a.y > H + a.radius) a.y = -a.radius;
        }
    }

    function drawBg(ctx, W, H) {
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, W, H);
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1;
        for (var i = 0; i < bgAsteroids.length; i++) {
            var a = bgAsteroids[i];
            ctx.globalAlpha = a.alpha;
            ctx.beginPath();
            var c = Math.cos(a.rot), s = Math.sin(a.rot);
            for (var j = 0; j < a.shape.length; j++) {
                var p = a.shape[j];
                var px = a.x + p.x * c - p.y * s;
                var py = a.y + p.x * s + p.y * c;
                if (j === 0) ctx.moveTo(px, py);
                else ctx.lineTo(px, py);
            }
            ctx.closePath();
            ctx.stroke();
        }
        ctx.globalAlpha = 1;
    }

    // --- Overlay helpers ---
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
        hud.style.display = show ? "flex" : "none";
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

    var mouseAttached = false;
    var lastW = 900, lastH = 800;

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

    function activeScreenId() {
        if (currentName === "paused") return "pause";
        if (currentName === "playing") return "";
        return currentName;
    }

    function ensureMouse() {
        if (mouseAttached) return;
        if (!overlay) overlay = document.getElementById("overlay");
        if (!overlay) return;
        mouseAttached = true;
        overlay.addEventListener("mousemove", function(e) {
            var sid = activeScreenId();
            if (!sid) return;
            var t = findMenuItem(e.target, overlay);
            if (!t) return;
            var items = getMenuItems(sid);
            var i = items.indexOf(t);
            if (i >= 0 && menuIndex !== i) {
                menuIndex = i;
                updateSelection(sid);
                A.Audio.sfxMenuMove();
            }
        });
        overlay.addEventListener("click", function(e) {
            var sid = activeScreenId();
            if (!sid) return;
            var t = findMenuItem(e.target, overlay);
            if (!t) return;
            var items = getMenuItems(sid);
            var i = items.indexOf(t);
            if (i < 0) return;
            menuIndex = i;
            updateSelection(sid);
            keydown("Enter", lastW, lastH);
        });
    }

    function menuNav(screenId, key, onSelect) {
        var items = getMenuItems(screenId);
        if (!items.length) return;
        if (key === "ArrowUp") {
            menuIndex = (menuIndex - 1 + items.length) % items.length;
            updateSelection(screenId);
            A.Audio.sfxMenuMove();
        } else if (key === "ArrowDown") {
            menuIndex = (menuIndex + 1) % items.length;
            updateSelection(screenId);
            A.Audio.sfxMenuMove();
        } else if (key === "Enter") {
            A.Audio.sfxMenuSelect();
            if (onSelect) onSelect(items[menuIndex]);
        }
    }

    // --- HUD update ---
    function updateHud() {
        var s = A.Game.getState();
        if (!s) return;
        document.getElementById("hud-score").textContent = String(s.score);
        document.getElementById("hud-hi").textContent = String(A.Storage.highScore);
        document.getElementById("hud-wave").textContent = String(s.wave);
        document.getElementById("hud-lives").textContent = String(s.lives);
    }

    // --- Switching ---
    function switchTo(name, W, H) {
        currentName = name;
        menuIndex = 0;
        if (W) lastW = W;
        if (H) lastH = H;
        ensureMouse();
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
            A.Game.start(W || 900, H || 800);
            updateHud();
        } else if (name === "paused") {
            showOverlay("pause");
            updateSelection("pause");
            A.Game.setPaused(true);
        } else if (name === "gameover") {
            var s = A.Game.getState();
            var isHi = A.Storage.maybeUpdate(s ? s.score : 0);
            var lines = [];
            lines.push("SCORE   " + (s ? s.score : 0));
            lines.push("WAVE    " + (s ? s.wave : 1));
            lines.push("HI      " + A.Storage.highScore);
            if (isHi) lines.push("");
            if (isHi) lines.push("NEW HIGH SCORE!");
            document.getElementById("gameover-stats").textContent = lines.join("\n");
            showOverlay("gameover");
            showHud(false);
            updateSelection("gameover");
        }
    }

    // --- Key handling ---
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
                A.Game.setPaused(false);
                hideOverlay();
                showHud(true);
                currentName = "playing";
                return;
            }
            menuNav("pause", key, function(item) {
                var act = item.getAttribute("data-action");
                if (act === "resume") {
                    A.Game.setPaused(false);
                    hideOverlay();
                    showHud(true);
                    currentName = "playing";
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
        }
    }

    function keyup(_key) { /* lib/input samples state each frame */ }

    function update(dt, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            updateBg(dt, W, H);
        } else if (currentName === "playing") {
            A.Game.update(dt, W, H);
            if (A.FX) A.FX.update(dt);
            if (A.Game.isGameOver()) {
                switchTo("gameover");
                return;
            }
            updateHud();
        } else if (currentName === "paused" || currentName === "gameover") {
            // freeze particles visually but let them finish gently? keep them fixed.
        }
    }

    function draw(ctx, W, H) {
        if (currentName === "title" || currentName === "howtoplay") {
            drawBg(ctx, W, H);
        } else if (currentName === "playing" || currentName === "paused" || currentName === "gameover") {
            ctx.fillStyle = "#000000";
            ctx.fillRect(0, 0, W, H);
            A.Game.draw(ctx, W, H);
            if (A.FX) A.FX.draw(ctx, W, H);
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
