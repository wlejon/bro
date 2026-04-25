// screens.js — screen manager + gameplay state for Serpcoil.
var SC = SC || {};

SC.Game = (function () {
    "use strict";

    // --- Runtime state ---
    var state = {
        canvas: null, ctx: null,
        W: 1280, H: 800,
        path: null, chain: null, shooter: null,
        level: 0,           // 0-based index
        score: 0,
        combo: 1,           // displayed multiplier (max cascade depth this window)
        cascadeDepth: 0,    // pops in the current cascade window
        comboOrbs: 0,       // orbs popped this combo window
        comboTimer: 0,      // ms remaining on combo decay
        popsCounted: 0,
        spawnTicker: 0,
        danger: false,
        dangerPrev: false,
        mouseX: 640, mouseY: 400,
        aimKeyX: 0, aimKeyY: 0, // keyboard aim deltas
        storage: null,
        progress: null,    // { unlocked: n, stars: {...}, bestScore: {...} }
        settings: null,    // { sfxVol, musicVol, showTrails }
        active: false,     // is gameplay simulating?
        ended: null,       // "won" | "lost" | null
        levelClearBonus: 0
    };

    // Exposed to test.js
    function hooks() {
        return {
            fire: function (angle) {
                if (angle != null) state.shooter.setAim(angle);
                return state.shooter.fire();
            },
            chain: function () { return state.chain; },
            shooter: function () { return state.shooter; },
            path: function () { return state.path; },
            insertAt: function (d, color) {
                var idx = state.chain.insertAt(d, color);
                handlePopAt(idx);
                return idx;
            },
            detectMatches: function (idx) { return state.chain.detectMatches(idx); },
            seedLevel: function (n, seed) {
                startLevel(n, seed);
            },
            score: function () { return state.score; },
            setScore: function (v) { state.score = v; },
            combo: function () { return state.combo; },
            danger: function () { return state.danger; },
            state: function () { return state; },
            isActive: function () { return state.active; },
            forceEmpty: function () { state.chain.forceEmpty(); },
            advanceChainToGoal: function () {
                // Push head to the goal — triggers lose on next tick.
                var orbs = state.chain.orbs();
                if (orbs.length) orbs[orbs.length - 1].d = state.path.length();
            },
            setChainSpeed: function (s) { state.chain.speed(s); },
            tick: function (dt) { tick(dt); },
            currentScreen: function () { return Screens.currentName(); },
            awardPowerup: function (pu) {
                state.shooter.setCurrent(state.shooter.current(), pu);
            }
        };
    }

    // --- Scoring ---
    function scoreForPop(count, comboDepth) {
        // base 10/orb, 1.5x if pop-group > 3, combo multiplier.
        var base = count * 10;
        if (count >= 4) base = Math.floor(base * 1.5);
        var mult = [1, 2, 4, 8, 12, 16][Math.min(5, comboDepth)];
        return base * mult;
    }

    // Fire events from pops to update score, fx, audio, combo. Called
    // for every pop — both the initial insert-triggered match AND each
    // follow-up pop that occurs after a merge closes a gap.
    function onPopGroup(popped, _ignored, positions) {
        var color = popped[0] ? popped[0].color : 1;
        // Cascade tracking: if combo timer is still alive, this pop is
        // part of an ongoing cascade. Otherwise it starts a fresh one.
        if (state.comboTimer <= 0) state.cascadeDepth = 0;
        state.cascadeDepth += 1;
        if (state.cascadeDepth > state.combo) state.combo = state.cascadeDepth;
        state.comboTimer = 1800;

        var gain = scoreForPop(popped.length, state.cascadeDepth);
        state.score += gain;
        state.popsCounted += popped.length;

        for (var i = 0; i < positions.length; i++) {
            SC.FX.burst(positions[i].x, positions[i].y, positions[i].color, 14);
        }
        var p0 = positions[0];
        if (p0) {
            SC.FX.floatText(p0.x, p0.y - 10, "+" + gain, "#ffd86b");
            SC.FX.shockwave(p0.x, p0.y, { maxR: 80 });
        }
        SC.Audio.sfxPop(color, state.cascadeDepth);

        // Chance to award powerup onto next slot.
        if (state.shooter.maybeInjectPU()) {
            SC.FX.floatText(state.shooter.x(), state.shooter.y() - 40,
                "POWERUP!", "#b56dff");
            SC.Audio.sfxPowerup();
        }
    }

    // Pop directly at idx (used by insert / colorshift). Follow-up combo
    // pops are handled inside chain.tick when segments merge.
    function handlePopAt(idx) {
        return state.chain.popAround(idx, onPopGroup);
    }

    // --- Projectile / insertion ---
    function tryInsertProjectile(proj) {
        // Match the projectile against every orb in the chain, then against path.
        var orbs = state.chain.orbs();
        var diam = state.chain.ORB_DIAM;
        var hitRadius2 = (diam * 0.85) * (diam * 0.85);
        for (var i = 0; i < orbs.length; i++) {
            var p = state.path.pointAt(orbs[i].d);
            var dx = p.x - proj.x, dy = p.y - proj.y;
            if (dx*dx + dy*dy <= hitRadius2) {
                // Insert just behind or just in front of this orb based on dot product.
                var tangent = state.path.tangentAt(orbs[i].d);
                var dot = dx * tangent.x + dy * tangent.y; // orb→proj projected onto tangent
                // If proj is "ahead" of orb (dot < 0 means proj is backward along path)
                var insertD;
                if (dot > 0) insertD = orbs[i].d - diam * 0.5;
                else         insertD = orbs[i].d + diam * 0.5;
                if (insertD < 0) insertD = 0;
                applyProjectileEffect(proj, insertD, i);
                return true;
            }
        }
        return false;
    }

    function applyProjectileEffect(proj, insertD, hitIdx) {
        state.shooter.removeProjectile(proj);
        if (proj.pu === SC.Shooter.PU_BACKTRACK) {
            state.chain.backtrack(160);
            SC.Audio.sfxPowerup();
            SC.FX.shockwave(proj.x, proj.y, { maxR: 140, color: "#56d8ff" });
            SC.FX.floatText(proj.x, proj.y, "BACKTRACK", "#56d8ff");
            return;
        }
        if (proj.pu === SC.Shooter.PU_BLASTER) {
            var res = state.chain.blastAt(proj.x, proj.y, 80);
            for (var i = 0; i < res.positions.length; i++) {
                SC.FX.burst(res.positions[i].x, res.positions[i].y, res.positions[i].color, 12);
            }
            state.score += res.popped.length * 25;
            SC.FX.shockwave(proj.x, proj.y, { maxR: 180, color: "#e63946" });
            SC.FX.floatText(proj.x, proj.y, "+" + (res.popped.length * 25), "#ffd86b");
            SC.Audio.sfxPowerup();
            return;
        }
        if (proj.pu === SC.Shooter.PU_COLORSHIFT) {
            var n = state.chain.colorshift(hitIdx, proj.color);
            SC.FX.floatText(proj.x, proj.y, "SHIFTED x" + n, "#e9c46a");
            SC.Audio.sfxPowerup();
            // Check for matches at hitIdx now.
            handlePopAt(hitIdx);
            return;
        }
        if (proj.pu === SC.Shooter.PU_SLOWMO) {
            state.chain.setSlowmo(6000);
            SC.FX.shockwave(proj.x, proj.y, { maxR: 200, color: "#4cc9f0" });
            SC.FX.floatText(proj.x, proj.y, "SLOW-MO", "#4cc9f0");
            SC.Audio.sfxPowerup();
            return;
        }
        // Normal orb — insert and detect.
        var idx = state.chain.insertAt(insertD, proj.color);
        SC.Audio.sfxInsert();
        handlePopAt(idx);
    }

    // --- Level setup ---
    function startLevel(levelIdx, seed) {
        state.level = levelIdx;
        state.score = 0;
        state.combo = 1;
        state.cascadeDepth = 0;
        state.comboOrbs = 0;
        state.comboTimer = 0;
        state.popsCounted = 0;
        state.ended = null;
        state.levelClearBonus = 0;

        var L = SC.Levels.scaled(levelIdx, state.W, state.H);
        state.path = SC.Path.create(L.controls);
        var rng = seed != null ? makeRng(seed) : Math.random;
        state.chain = SC.Chain.create({
            path: state.path,
            palette: L.palette,
            totalToSpawn: L.totalOrbs,
            speed: L.chainSpeed,
            rng: rng
        });
        state.shooter = SC.Shooter.create({
            x: L.shooter.x, y: L.shooter.y,
            palette: L.palette,
            rng: rng
        });
        state.active = true;
        SC.FX.clear();
    }

    function makeRng(seed) {
        var s = (seed >>> 0) || 1;
        return function () {
            s = (s * 1664525 + 1013904223) >>> 0;
            return s / 0xffffffff;
        };
    }

    // --- Tick ---
    function tick(dt) {
        if (!state.active) return;
        // Aim: mouse or keyboard
        if (state.aimKeyX !== 0 || state.aimKeyY !== 0) {
            var a = state.shooter.aim() + state.aimKeyX * 0.005 * dt;
            state.shooter.setAim(a);
        } else {
            state.shooter.aimAt(state.mouseX, state.mouseY);
        }
        state.chain.tick(dt, onPopGroup);
        state.shooter.tick(dt);

        // Danger enter transition
        var d = state.chain.dangerActive();
        if (d && !state.dangerPrev) {
            SC.Audio.sfxDanger();
        }
        state.dangerPrev = d;
        state.danger = d;

        // Projectile / chain collision
        var projectiles = state.shooter.projectiles();
        var chainOrbs = state.chain.orbs();
        for (var i = projectiles.length - 1; i >= 0; i--) {
            var proj = projectiles[i];
            if (proj.x < -50 || proj.x > state.W + 50 ||
                proj.y < -50 || proj.y > state.H + 50) {
                state.shooter.removeProjectile(proj);
                continue;
            }
            if (chainOrbs.length > 0) {
                tryInsertProjectile(proj);
            }
        }

        // Once the spawn queue is empty, the shooter should only offer
        // colors that still exist on the board. If the player's loaded
        // current/next ball is now a color the previous shot cleared,
        // dissolve it (burst fx) and replace with a still-living color.
        if (state.chain.remainingToSpawn() === 0) {
            var live = state.chain.colorsRemaining();
            if (live.length > 0) {
                state.shooter.setPalette(live);
                var sx = state.shooter.x(), sy = state.shooter.y();
                var pickLive = function () { return live[(Math.random() * live.length) | 0]; };
                if (live.indexOf(state.shooter.current()) < 0) {
                    SC.FX.burst(sx, sy, state.shooter.current(), 14);
                    state.shooter.setCurrent(pickLive(), state.shooter.currentPU());
                }
                if (live.indexOf(state.shooter.next()) < 0) {
                    var ang = state.shooter.aim();
                    var bx = sx - Math.cos(ang) * 38, by = sy - Math.sin(ang) * 38;
                    SC.FX.burst(bx, by, state.shooter.next(), 10);
                    state.shooter.setNext(pickLive(), state.shooter.nextPU());
                }
            }
        }

        // Combo decay
        if (state.comboTimer > 0) {
            state.comboTimer -= dt;
            if (state.comboTimer <= 0) {
                state.combo = 1;
                state.cascadeDepth = 0;
            }
        }

        // Mouth puff spawn events.
        if (state.chain.remainingToSpawn() > 0) {
            state.spawnTicker += dt;
            if (state.spawnTicker > 260) {
                state.spawnTicker = 0;
                var p0 = state.path.pointAt(0);
                SC.FX.puff(p0.x, p0.y);
            }
        }

        SC.FX.update(dt);

        // Win / lose checks
        if (!state.ended) {
            if (state.chain.isComplete()) {
                state.ended = "won";
                state.active = false;
                onLevelWin();
            } else if (state.chain.headReachedGoal && state.chain.headReachedGoal()) {
                // headReachedGoal isn't exported; use direct test
            }
            // Inline lose check
            var orbs = state.chain.orbs();
            if (orbs.length > 0 && orbs[orbs.length - 1].d >= state.path.length()) {
                state.ended = "lost";
                state.active = false;
                onLevelLose();
            }
        }

        updateHUD();
    }

    function onLevelWin() {
        // Level clear bonus based on speed and remaining fires.
        var bonus = 500 + state.level * 100;
        state.levelClearBonus = bonus;
        state.score += bonus;
        SC.Audio.sfxClear();
        // Persist progress
        var stars = computeStars();
        persistLevelResult(stars);
        Screens.switchTo("levelclear");
    }

    function onLevelLose() {
        SC.Audio.sfxGameOver();
        persistHighScore();
        Screens.switchTo("gameover");
    }

    function computeStars() {
        // 3 stars if fewer than ~15% of orbs ever reached past 70%,
        // 2 if under danger threshold cleared, 1 otherwise.
        // Simple proxy: score thresholds by level.
        var L = SC.Levels.get(state.level);
        var perfect = L.totalOrbs * 20;
        var s = state.score;
        if (s >= perfect * 1.2) return 3;
        if (s >= perfect * 0.8) return 2;
        return 1;
    }

    function persistLevelResult(stars) {
        if (!state.progress) return;
        var prev = state.progress.stars[state.level] || 0;
        if (stars > prev) state.progress.stars[state.level] = stars;
        var prevScore = state.progress.bestScore[state.level] || 0;
        if (state.score > prevScore) state.progress.bestScore[state.level] = state.score;
        if (state.progress.unlocked <= state.level + 1 && state.level + 1 < SC.Levels.count()) {
            state.progress.unlocked = state.level + 2;
        }
        state.storage.set("unlocked", state.progress.unlocked);
        state.storage.save();
        persistHighScore();
    }

    function persistHighScore() {
        try {
            var hs = Storage.highscores("serpcoil", 10);
            hs.add({
                score: state.score,
                level: state.level + 1,
                date: new Date().toISOString().slice(0, 10)
            });
        } catch (e) {}
    }

    // --- Draw ---
    function draw() {
        var ctx = state.ctx;
        var W = state.W, H = state.H;
        // Background
        ctx.fillStyle = "#070412";
        ctx.fillRect(0, 0, W, H);
        // Subtle starfield
        drawStars(ctx, W, H);

        if (state.path) {
            state.path.draw(ctx);
            // Mouth
            var m = state.path.pointAt(0);
            drawMouth(ctx, m.x, m.y);
            // Goal
            var g = state.path.pointAt(state.path.length());
            drawGoal(ctx, g.x, g.y, state.danger);
        }

        if (state.chain) state.chain.draw(ctx);
        if (state.shooter) state.shooter.draw(ctx);
        SC.FX.draw(ctx);
    }

    var stars = null;
    function drawStars(ctx, W, H) {
        if (!stars) {
            stars = [];
            for (var i = 0; i < 80; i++) {
                stars.push({
                    x: Math.random() * 1280,
                    y: Math.random() * 800,
                    a: 0.2 + Math.random() * 0.4
                });
            }
        }
        for (var j = 0; j < stars.length; j++) {
            var s = stars[j];
            ctx.globalAlpha = s.a;
            ctx.fillStyle = "#6a4aa0";
            ctx.fillRect(s.x * (W / 1280), s.y * (H / 800), 1.5, 1.5);
        }
        ctx.globalAlpha = 1.0;
    }

    function drawMouth(ctx, x, y) {
        ctx.save();
        var grad = ctx.createRadialGradient(x, y, 6, x, y, 40);
        grad.addColorStop(0, "rgba(180,100,255,0.6)");
        grad.addColorStop(1, "rgba(180,100,255,0)");
        ctx.fillStyle = grad;
        ctx.fillRect(x - 40, y - 40, 80, 80);
        ctx.fillStyle = "#1a0e2e";
        ctx.beginPath();
        ctx.arc(x, y, 22, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = "#9a56ff";
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.restore();
    }

    function drawGoal(ctx, x, y, danger) {
        ctx.save();
        var time = performance.now() * 0.004;
        var pulse = Math.sin(time * (danger ? 6 : 2)) * 0.5 + 0.5;
        var col = danger ? "#ff5a5a" : "#ffd86b";
        ctx.strokeStyle = col;
        ctx.lineWidth = 3;
        ctx.globalAlpha = 0.6 + pulse * 0.4;
        ctx.beginPath();
        ctx.arc(x, y, 28 + pulse * 6, 0, Math.PI * 2);
        ctx.stroke();
        ctx.lineWidth = 1.5;
        ctx.globalAlpha = 1;
        ctx.beginPath();
        ctx.arc(x, y, 18, 0, Math.PI * 2);
        ctx.stroke();
        // Inner fill
        ctx.fillStyle = danger ? "#2a0a0a" : "#0a0618";
        ctx.fill();
        ctx.restore();
    }

    // --- HUD ---
    function updateHUD() {
        Hud.text("#hud-score", String(state.score));
        Hud.text("#hud-level", String(state.level + 1));
        Hud.text("#hud-combo", "x" + state.combo);
        var left = state.chain.remainingToSpawn() + state.chain.count();
        Hud.text("#hud-left", String(left));
        var total = state.chain.totalToSpawn();
        var pct = total > 0 ? Math.max(0, Math.min(100, (1 - left / total) * 100)) : 0;
        var fill = document.getElementById("hud-progress-fill");
        if (fill) fill.style.width = pct + "%";
        var dEl = document.getElementById("hud-danger");
        if (dEl) dEl.style.display = state.danger ? "block" : "none";
    }

    // --- Input ---
    function onMouseMove(e) {
        state.mouseX = e.clientX;
        state.mouseY = e.clientY;
    }

    function onMouseDown(e) {
        if (!state.active) return;
        if (e.button === 0) {
            var p = state.shooter.fire();
            if (p) SC.Audio.sfxShoot();
        } else if (e.button === 2) {
            state.shooter.swap();
            SC.Audio.sfxSwap();
        }
    }

    function onContextMenu(e) {
        e.preventDefault();
        if (state.active) {
            state.shooter.swap();
            SC.Audio.sfxSwap();
        }
        return false;
    }

    function onKeyDown(e) {
        var k = e.key;
        if (Screens.currentName() === "play") {
            if (k === "Escape" || k === "p" || k === "P") {
                Screens.switchTo("pause");
                return;
            }
            if (!state.active) return;
            if (k === " ") {
                var p = state.shooter.fire();
                if (p) SC.Audio.sfxShoot();
            } else if (k === "Shift") {
                state.shooter.swap();
                SC.Audio.sfxSwap();
            } else if (k === "ArrowLeft") state.aimKeyX = -1;
            else if (k === "ArrowRight") state.aimKeyX = 1;
            else if (k === "ArrowUp") state.aimKeyY = -1;
            else if (k === "ArrowDown") state.aimKeyY = 1;
            return;
        }
        // Menu screens
        Screens.keydown(k);
    }

    function onKeyUp(e) {
        var k = e.key;
        if (Screens.currentName() === "play") {
            if (k === "ArrowLeft" && state.aimKeyX === -1) state.aimKeyX = 0;
            else if (k === "ArrowRight" && state.aimKeyX === 1) state.aimKeyX = 0;
            else if (k === "ArrowUp" && state.aimKeyY === -1) state.aimKeyY = 0;
            else if (k === "ArrowDown" && state.aimKeyY === 1) state.aimKeyY = 0;
        }
    }

    // --- Init ---
    function init() {
        state.canvas = document.getElementById("game");
        state.ctx = state.canvas.getContext("2d");
        state.W = state.canvas.width || window.innerWidth || 1280;
        state.H = state.canvas.height || window.innerHeight || 800;

        // Storage
        state.storage = Storage.create("serpcoil");
        state.storage.load({
            sfxVol: 80, musicVol: 70, showTrails: true,
            unlocked: 1,
            stars: {},     // idx → 0..3
            bestScore: {}  // idx → number
        });
        state.progress = {
            unlocked: state.storage.get("unlocked"),
            stars: state.storage.get("stars") || {},
            bestScore: state.storage.get("bestScore") || {}
        };
        // Self-heal: any cleared level (stars > 0) must unlock its successor.
        // Recovers from past saves where the unlock counter wasn't persisted.
        for (var lk in state.progress.stars) {
            if (state.progress.stars[lk] > 0) {
                var minUnlock = (parseInt(lk, 10) | 0) + 2;
                if (minUnlock > state.progress.unlocked &&
                    minUnlock <= SC.Levels.count()) {
                    state.progress.unlocked = minUnlock;
                }
            }
        }
        state.storage.set("unlocked", state.progress.unlocked);
        state.storage.save();
        // Settings ref shares the same object tree — so updates persist via save()
        state.settings = {
            sfxVol: state.storage.get("sfxVol"),
            musicVol: state.storage.get("musicVol"),
            showTrails: state.storage.get("showTrails")
        };

        SFX.init({
            sfxVol: state.settings.sfxVol / 100,
            musicVol: state.settings.musicVol / 100
        });

        // Canvas sizing tracks viewport (bro scales automatically; read on first frame).
        updateCanvasSize();

        // Bind global input
        document.body.addEventListener("mousemove", onMouseMove);
        document.body.addEventListener("mousedown", onMouseDown);
        document.body.addEventListener("contextmenu", onContextMenu);
        document.body.addEventListener("keydown", onKeyDown);
        document.body.addEventListener("keyup", onKeyUp);

        Screens.init();
        Screens.switchTo("title");
    }

    function updateCanvasSize() {
        var w = state.ctx.canvasWidth || state.canvas.width || window.innerWidth || 1280;
        var h = state.ctx.canvasHeight || state.canvas.height || window.innerHeight || 800;
        state.W = w;
        state.H = h;
    }

    // --- Screens manager (local, app-specific) ---
    var Screens = (function () {
        var current = null;
        var currentName = "";
        var menuIndex = 0;
        var overlay = null;
        var activeDOM = "";
        var backTarget = "title";
        var levelsUI = false;

        function show(id) {
            if (!overlay) overlay = document.getElementById("overlay");
            var kids = overlay.children;
            for (var i = 0; i < kids.length; i++) kids[i].style.display = "none";
            var el = document.getElementById("screen-" + id);
            if (el) el.style.display = "block";
            overlay.style.display = "block";
            activeDOM = id;
            document.getElementById("hud").style.display = "none";
        }

        function hide() {
            if (!overlay) overlay = document.getElementById("overlay");
            overlay.style.display = "none";
        }

        function items(id) {
            var el = document.getElementById("screen-" + id);
            if (!el) return [];
            var list = [];
            var boxes = el.querySelectorAll(".menu-items");
            for (var bi = 0; bi < boxes.length; bi++) {
                var kids = boxes[bi].children;
                for (var i = 0; i < kids.length; i++) {
                    if ((kids[i].className || "").indexOf("menu-item") >= 0) list.push(kids[i]);
                }
            }
            return list;
        }

        function refreshSel(id) {
            var list = items(id);
            for (var i = 0; i < list.length; i++) {
                list[i].className = (i === menuIndex) ? "menu-item selected" : "menu-item";
            }
        }

        function nav(id, key, onSelect, opts) {
            opts = opts || {};
            var list = items(id);
            if (key === "ArrowUp") {
                menuIndex = (menuIndex - 1 + list.length) % list.length;
                refreshSel(id); SC.Audio.sfxMenu();
            } else if (key === "ArrowDown") {
                menuIndex = (menuIndex + 1) % list.length;
                refreshSel(id); SC.Audio.sfxMenu();
            } else if (key === "Enter") {
                SC.Audio.sfxSelect();
                if (onSelect) onSelect(menuIndex, list[menuIndex]);
            } else if (key === "ArrowLeft" && opts.onAdjust) {
                opts.onAdjust(-1);
            } else if (key === "ArrowRight" && opts.onAdjust) {
                opts.onAdjust(1);
            } else if (key === "Escape" && opts.onBack) {
                opts.onBack();
            }
        }

        // --- LEVEL SELECT UI ---
        function buildLevelGrid() {
            var grid = document.getElementById("level-grid");
            if (!grid) return;
            grid.innerHTML = "";
            var unlocked = state.progress.unlocked;
            for (var i = 0; i < SC.Levels.count(); i++) {
                var locked = (i + 1) > unlocked;
                var node = document.createElement("div");
                node.className = "level-node" + (locked ? " locked" : "");
                var stars = state.progress.stars[i] || 0;
                var best = state.progress.bestScore[i] || 0;
                node.innerHTML =
                    '<div class="level-num">' + (i + 1) + '</div>' +
                    '<div class="level-stars">' + (stars > 0 ? "★".repeat(stars) : "&nbsp;") + '</div>' +
                    '<div class="level-score">' + (best > 0 ? best : "") + '</div>';
                (function (idx, lk) {
                    node.addEventListener("click", function () {
                        if (lk) return;
                        SC.Audio.sfxSelect();
                        startLevel(idx);
                        switchTo("play");
                    });
                })(i, locked);
                grid.appendChild(node);
            }
        }

        // --- REFRESH SETTINGS DISPLAY ---
        function refreshSettings() {
            var S = state.settings;
            var el;
            el = document.getElementById("opt-sfxVol"); if (el) el.textContent = S.sfxVol;
            el = document.getElementById("opt-musicVol"); if (el) el.textContent = S.musicVol;
            el = document.getElementById("opt-showTrails"); if (el) el.textContent = S.showTrails ? "ON" : "OFF";
        }

        function saveSettings() {
            state.storage.set("sfxVol", state.settings.sfxVol);
            state.storage.set("musicVol", state.settings.musicVol);
            state.storage.set("showTrails", state.settings.showTrails);
            state.storage.save();
        }

        function adjustSetting(dir) {
            var list = items("settings");
            if (menuIndex >= list.length) return;
            var key = list[menuIndex].getAttribute("data-setting");
            if (!key) return;
            var S = state.settings;
            if (key === "sfxVol") {
                S.sfxVol = Math.max(0, Math.min(100, S.sfxVol + dir * 10));
                SFX.setSfxVol(S.sfxVol / 100);
            } else if (key === "musicVol") {
                S.musicVol = Math.max(0, Math.min(100, S.musicVol + dir * 10));
                SFX.setMusicVol(S.musicVol / 100);
            } else if (key === "showTrails") {
                S.showTrails = !S.showTrails;
            }
            saveSettings();
            refreshSettings();
            SC.Audio.sfxMenu();
        }

        // --- HIGH SCORES ---
        function refreshHighScores() {
            var el = document.getElementById("hs-list");
            if (!el) return;
            try {
                var hs = Storage.highscores("serpcoil", 10);
                var list = hs.list();
                if (!list || list.length === 0) {
                    el.textContent = "No scores yet";
                    return;
                }
                var lines = [];
                for (var i = 0; i < list.length; i++) {
                    var s = list[i];
                    var rank = (i + 1).toString();
                    if (rank.length < 2) rank = " " + rank;
                    lines.push(rank + ". " + s.score + "   Lv" + s.level + "   " + (s.date || ""));
                }
                el.textContent = lines.join("\n");
            } catch (e) {
                el.textContent = "No scores yet";
            }
        }

        // --- LEVEL CLEAR ---
        function buildLevelClear() {
            var stars = computeStars();
            var starsEl = document.getElementById("clear-stars");
            if (starsEl) starsEl.textContent = "★".repeat(stars) + "☆".repeat(3 - stars);
            var st = document.getElementById("clear-stats");
            if (st) {
                st.textContent = "Score: " + state.score + "\n" +
                                 "Level: " + (state.level + 1) + "\n" +
                                 "Clear bonus: +" + state.levelClearBonus;
            }
        }

        function buildGameOver() {
            var st = document.getElementById("gameover-stats");
            if (st) {
                st.textContent = "Score: " + state.score + "\n" +
                                 "Level: " + (state.level + 1);
            }
        }

        // --- Screen definitions ---
        var defs = {};

        defs.title = {
            enter: function () { menuIndex = 0; show("title"); refreshSel("title"); },
            exit: function () {},
            keydown: function (k) {
                nav("title", k, function (idx) {
                    var list = items("title");
                    var act = list[idx].getAttribute("data-action");
                    if (act === "play") { startLevel(0); switchTo("play"); }
                    else if (act === "levelselect") switchTo("levelselect");
                    else if (act === "highscores") switchTo("highscores");
                    else if (act === "howtoplay") switchTo("howtoplay");
                    else if (act === "settings") { backTarget = "title"; switchTo("settings"); }
                    else if (act === "credits") switchTo("credits");
                });
            }
        };

        defs.levelselect = {
            enter: function () { menuIndex = 0; show("levelselect"); buildLevelGrid(); refreshSel("levelselect"); },
            exit: function () {},
            keydown: function (k) {
                nav("levelselect", k, function () { switchTo("title"); },
                    { onBack: function () { switchTo("title"); } });
            }
        };

        defs.play = {
            enter: function () {
                hide();
                document.getElementById("hud").style.display = "block";
                state.active = true;
            },
            exit: function () {},
            keydown: function () {},
            update: function (dt) { tick(dt); }
        };

        defs.pause = {
            enter: function () { menuIndex = 0; show("pause"); refreshSel("pause"); state.active = false; },
            exit: function () {},
            keydown: function (k) {
                nav("pause", k, function (idx) {
                    var act = items("pause")[idx].getAttribute("data-action");
                    if (act === "resume") switchTo("play");
                    else if (act === "restart") { startLevel(state.level); switchTo("play"); }
                    else if (act === "settings") { backTarget = "pause"; switchTo("settings"); }
                    else if (act === "quit") switchTo("title");
                }, { onBack: function () { switchTo("play"); } });
            }
        };

        defs.levelclear = {
            enter: function () { menuIndex = 0; show("levelclear"); buildLevelClear(); refreshSel("levelclear"); },
            exit: function () {},
            keydown: function (k) {
                nav("levelclear", k, function (idx) {
                    var act = items("levelclear")[idx].getAttribute("data-action");
                    if (act === "next") {
                        var nxt = state.level + 1;
                        if (nxt >= SC.Levels.count()) switchTo("title");
                        else { startLevel(nxt); switchTo("play"); }
                    } else if (act === "retry") { startLevel(state.level); switchTo("play"); }
                    else if (act === "quit") switchTo("title");
                });
            }
        };

        defs.gameover = {
            enter: function () { menuIndex = 0; show("gameover"); buildGameOver(); refreshSel("gameover"); },
            exit: function () {},
            keydown: function (k) {
                nav("gameover", k, function (idx) {
                    var act = items("gameover")[idx].getAttribute("data-action");
                    if (act === "retry") { startLevel(state.level); switchTo("play"); }
                    else if (act === "quit") switchTo("title");
                });
            }
        };

        defs.highscores = {
            enter: function () { menuIndex = 0; show("highscores"); refreshHighScores(); refreshSel("highscores"); },
            exit: function () {},
            keydown: function (k) {
                nav("highscores", k, function () { switchTo("title"); },
                    { onBack: function () { switchTo("title"); } });
            }
        };

        defs.howtoplay = {
            enter: function () { menuIndex = 0; show("howtoplay"); refreshSel("howtoplay"); },
            exit: function () {},
            keydown: function (k) {
                nav("howtoplay", k, function () { switchTo("title"); },
                    { onBack: function () { switchTo("title"); } });
            }
        };

        defs.settings = {
            enter: function () { menuIndex = 0; show("settings"); refreshSettings(); refreshSel("settings"); },
            exit: function () {},
            keydown: function (k) {
                nav("settings", k, function (idx) {
                    var list = items("settings");
                    var act = list[idx].getAttribute("data-action");
                    if (act === "back") switchTo(backTarget);
                    else adjustSetting(1);
                }, {
                    onAdjust: function (dir) { adjustSetting(dir); },
                    onBack: function () { switchTo(backTarget); }
                });
            }
        };

        defs.credits = {
            enter: function () { menuIndex = 0; show("credits"); refreshSel("credits"); },
            exit: function () {},
            keydown: function (k) {
                nav("credits", k, function () { switchTo("title"); },
                    { onBack: function () { switchTo("title"); } });
            }
        };

        function switchTo(name) {
            if (current && current.exit) current.exit();
            currentName = name;
            current = defs[name];
            if (current && current.enter) current.enter();
        }

        function init() {
            overlay = document.getElementById("overlay");
            // Mouse menu nav
            overlay.addEventListener("mousemove", function (e) {
                if (!activeDOM) return;
                var t = e.target;
                while (t && t !== overlay) {
                    if (t.className && t.className.indexOf("menu-item") >= 0) break;
                    t = t.parentNode;
                }
                if (!t || t === overlay) return;
                var list = items(activeDOM);
                for (var i = 0; i < list.length; i++) {
                    if (list[i] === t && menuIndex !== i) {
                        menuIndex = i;
                        refreshSel(activeDOM);
                        SC.Audio.sfxMenu();
                        break;
                    }
                }
            });
            overlay.addEventListener("click", function (e) {
                if (!activeDOM) return;
                var t = e.target;
                while (t && t !== overlay) {
                    if (t.className && t.className.indexOf("menu-item") >= 0) break;
                    t = t.parentNode;
                }
                if (!t || t === overlay) return;
                var list = items(activeDOM);
                for (var i = 0; i < list.length; i++) {
                    if (list[i] === t) {
                        menuIndex = i;
                        refreshSel(activeDOM);
                        if (current && current.keydown) current.keydown("Enter");
                        break;
                    }
                }
            });
        }

        return {
            init: init,
            switchTo: switchTo,
            currentName: function () { return currentName; },
            keydown: function (k) { if (current && current.keydown) current.keydown(k); },
            update: function (dt) { if (current && current.update) current.update(dt); }
        };
    })();

    return {
        init: init,
        draw: draw,
        tick: tick,
        Screens: Screens,
        hooks: hooks,
        updateCanvasSize: updateCanvasSize,
        state: function () { return state; },
        startLevel: startLevel
    };
})();
