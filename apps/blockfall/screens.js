// screens.js — Screen manager and all game screens
var T = T || {};

T.Screens = (function() {
    var current = null;
    var currentName = "";
    var overlay = null;
    var menuIndex = 0;
    var backTarget = "title"; // where Settings/Controls should return
    var activeScreenId = ""; // DOM screen ID currently shown (e.g. "title", "mode-select")

    // --- Helpers ---
    function showOverlay(screenId) {
        if (!overlay) overlay = document.getElementById("overlay");
        // Hide all screen divs
        var divs = overlay.children;
        for (var i = 0; i < divs.length; i++) divs[i].style.display = "none";
        var el = document.getElementById("screen-" + screenId);
        if (el) el.style.display = "block";
        overlay.style.display = "block";
        activeScreenId = screenId;
    }

    function hideOverlay() {
        if (!overlay) overlay = document.getElementById("overlay");
        overlay.style.display = "none";
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

    function menuNav(screenId, key, onSelect, opts) {
        opts = opts || {};
        var items = getMenuItems(screenId);
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
            if (onSelect) onSelect(menuIndex, items[menuIndex]);
        } else if (key === "ArrowLeft" && opts.onAdjust) {
            opts.onAdjust(-1);
        } else if (key === "ArrowRight" && opts.onAdjust) {
            opts.onAdjust(1);
        } else if (key === "Escape" && opts.onBack) {
            opts.onBack();
        }
    }

    // --- Title background animation ---
    var bgPieces = [];

    function initBgPieces() {
        bgPieces = [];
        for (var i = 0; i < 20; i++) {
            bgPieces.push({
                type: 1 + Math.floor(Math.random() * 7),
                x: Math.random() * 800,
                y: Math.random() * 700 - 700,
                rot: Math.floor(Math.random() * 4),
                speed: 15 + Math.random() * 25,
                alpha: 0.08 + Math.random() * 0.12
            });
        }
    }

    function updateBgPieces(dt, W, H) {
        for (var i = 0; i < bgPieces.length; i++) {
            var p = bgPieces[i];
            p.y += p.speed * dt / 1000;
            if (p.y > H + 100) {
                p.y = -100;
                p.x = Math.random() * W;
                p.type = 1 + Math.floor(Math.random() * 7);
                p.rot = Math.floor(Math.random() * 4);
            }
        }
    }

    function drawBgPieces(ctx, W, H) {
        ctx.fillStyle = "#06060a";
        ctx.fillRect(0, 0, W, H);
        var cellSize = 18;
        for (var i = 0; i < bgPieces.length; i++) {
            var p = bgPieces[i];
            var cells = T.PIECES[p.type][p.rot & 3];
            ctx.globalAlpha = p.alpha;
            ctx.fillStyle = T.COLORS[p.type];
            for (var j = 0; j < cells.length; j++) {
                ctx.fillRect(p.x + cells[j][1] * cellSize, p.y + cells[j][0] * cellSize,
                             cellSize - 1, cellSize - 1);
            }
        }
        ctx.globalAlpha = 1.0;
    }

    // --- Countdown state ---
    var countdownTimer = 0;
    var countdownPhase = 3;

    // --- High score tab ---
    var hsMode = "marathon";

    // =========================================================================
    // Screen definitions
    // =========================================================================
    var screens = {};

    // --- TITLE ---
    screens.title = {
        enter: function() {
            initBgPieces();
            menuIndex = 0;
            showOverlay("title");
            updateSelection("title");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        keydown: function(key) {
            menuNav("title", key, function(idx) {
                if (idx === 0) switchTo("modeSelect");
                else if (idx === 1) switchTo("highScores");
                else if (idx === 2) switchTo("howToPlay");
                else if (idx === 3) switchTo("credits");
                else if (idx === 4) window.close();
            });
        }
    };

    // --- MODE SELECT ---
    screens.modeSelect = {
        enter: function() {
            menuIndex = 0;
            showOverlay("mode-select");
            updateSelection("mode-select");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        keydown: function(key) {
            menuNav("mode-select", key, function(idx) {
                if (idx <= 2) {
                    var modes = ["marathon", "sprint", "ultra"];
                    T.Board.startGame(modes[idx]);
                    switchTo("countdown");
                } else if (idx === 3) {
                    backTarget = "modeSelect";
                    switchTo("settings");
                } else if (idx === 4) {
                    switchTo("title");
                }
            }, { onBack: function() { switchTo("title"); } });
        }
    };

    // --- SETTINGS ---
    screens.settings = {
        enter: function() {
            menuIndex = 0;
            showOverlay("settings");
            this.refreshDisplay();
            updateSelection("settings");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        refreshDisplay: function() {
            var s = T.Storage.settings;
            var el;
            el = document.getElementById("opt-startLevel");
            if (el) el.textContent = String(s.startLevel);
            el = document.getElementById("opt-sfxVol");
            if (el) el.textContent = String(s.sfxVol);
            el = document.getElementById("opt-musicVol");
            if (el) el.textContent = String(s.musicVol);
            el = document.getElementById("opt-ghostPiece");
            if (el) el.textContent = s.ghostPiece ? "ON" : "OFF";
            el = document.getElementById("opt-gridLines");
            if (el) el.textContent = s.gridLines ? "ON" : "OFF";
        },
        adjust: function(dir) {
            var items = getMenuItems("settings");
            if (menuIndex >= items.length) return;
            var setting = items[menuIndex].getAttribute("data-setting");
            if (!setting) return;
            var s = T.Storage.settings;
            if (setting === "startLevel") {
                s.startLevel = Math.max(1, Math.min(20, s.startLevel + dir));
            } else if (setting === "sfxVol") {
                s.sfxVol = Math.max(0, Math.min(100, s.sfxVol + dir * 10));
                T.Audio.updateSfxVolume();
            } else if (setting === "musicVol") {
                s.musicVol = Math.max(0, Math.min(100, s.musicVol + dir * 10));
                T.Audio.updateMusicVolume();
            } else if (setting === "ghostPiece") {
                s.ghostPiece = !s.ghostPiece;
            } else if (setting === "gridLines") {
                s.gridLines = !s.gridLines;
            }
            T.Storage.save();
            this.refreshDisplay();
            T.Audio.sfxMenuMove();
        },
        keydown: function(key) {
            var self = this;
            menuNav("settings", key, function(idx) {
                var items = getMenuItems("settings");
                if (items[idx] && items[idx].getAttribute("data-action") === "back") {
                    switchTo(backTarget);
                } else {
                    self.adjust(1);
                }
            }, {
                onAdjust: function(dir) { self.adjust(dir); },
                onBack: function() { switchTo(backTarget); }
            });
        }
    };

    // Controls screen removed — System → Input (F8) handles rebinding.

    function showHUD() {
        var el = document.getElementById("hud");
        if (el) el.style.display = "block";
    }
    function hideHUD() {
        var el = document.getElementById("hud");
        if (el) el.style.display = "none";
    }

    // --- COUNTDOWN ---
    screens.countdown = {
        enter: function() {
            hideOverlay();
            countdownTimer = 0;
            countdownPhase = 3;
            T.Audio.sfxCountdown();
            var el = document.getElementById("countdown-text");
            if (el) { el.textContent = "3"; el.style.display = "block"; el.style.color = "#4fc3f7"; }
        },
        exit: function() {
            var el = document.getElementById("countdown-text");
            if (el) el.style.display = "none";
        },
        update: function(dt, W, H) {
            countdownTimer += dt;
            var newPhase = 3 - Math.floor(countdownTimer / 700);
            if (newPhase < countdownPhase && newPhase >= 0) {
                countdownPhase = newPhase;
                var el = document.getElementById("countdown-text");
                if (countdownPhase > 0) {
                    T.Audio.sfxCountdown();
                    if (el) { el.textContent = String(countdownPhase); el.style.color = "#4fc3f7"; }
                } else {
                    T.Audio.sfxGo();
                    if (el) { el.textContent = "GO!"; el.style.color = "#00e676"; }
                }
            }
            if (countdownTimer >= 3200) {
                var songIdx = T.Audio.getSongForLevel(T.Board.level);
                T.Audio.buildSequences(songIdx);
                T.Audio.startMusic(T.Board.level);
                switchTo("playing");
            }
        },
        draw: function(ctx, W, H) {
            ctx.fillStyle = "#06060a";
            ctx.fillRect(0, 0, W, H);
            T.Board.calcLayout(W, H);
            T.Board.drawBoard(ctx);
            T.Board.drawPreviews(ctx);
        },
        keydown: function() {}
    };

    // --- PLAYING ---
    screens.playing = {
        enter: function() {
            hideOverlay();
            showHUD();
            T.Input.resetDAS();
            T.Board.updateHUD();
        },
        exit: function() {
            hideHUD();
        },
        update: function(dt, W, H) {
            var B = T.Board;
            var I = T.Input;
            B.gameTime += dt;

            // Ultra timer
            if (B.mode === "ultra") {
                B.modeTimer -= dt;
                if (B.checkModeEnd()) {
                    finishGame();
                    return;
                }
            }

            if (!B.cur) return;

            // DAS
            if (I.das.dir !== 0) {
                I.das.timer += dt;
                if (!I.das.active) {
                    if (I.das.timer >= I.das.delay) { I.das.active = true; I.das.timer = 0; }
                }
                if (I.das.active) {
                    while (I.das.timer >= I.das.arr) {
                        I.das.timer -= I.das.arr;
                        if (I.das.dir === -1) B.moveLeft();
                        else if (I.das.dir === 1) B.moveRight();
                    }
                }
            }

            // Soft drop repeat
            if (I.softDrop.active) {
                I.softDrop.timer += dt;
                while (I.softDrop.timer >= I.softDrop.rate) {
                    I.softDrop.timer -= I.softDrop.rate;
                    if (B.moveDown()) B.score += 1;
                }
            }

            // Gravity
            B.dropInterval = B.getDropInterval();
            B.dropTimer += dt;
            while (B.dropTimer >= B.dropInterval && !I.softDrop.active) {
                B.dropTimer -= B.dropInterval;
                B.moveDown();
            }

            // Lock delay
            if (B.cur && !B.canPlace(B.cur.type, B.cur.x, B.cur.y + 1, B.cur.rot)) {
                B.lockTimer += dt;
                if (B.lockTimer >= B.lockDelay) {
                    var lockResult = B.lockPiece();
                    if (lockResult === -1) { gameOver(); return; }
                    if (B.checkModeEnd()) { finishGame(); return; }
                    if (!B.spawnPiece()) { gameOver(); return; }
                }
            } else {
                B.lockTimer = 0;
            }

            T.FX.update(dt);
            T.Board.updateHUD();
        },
        draw: function(ctx, W, H) {
            ctx.fillStyle = "#06060a";
            ctx.fillRect(0, 0, W, H);
            T.Board.calcLayout(W, H);
            var shake = T.FX.getShakeOffset();
            ctx.save();
            ctx.translate(shake.x, shake.y);
            T.Board.drawBoard(ctx);
            T.Board.drawPreviews(ctx);
            T.FX.drawParticles(ctx);
            ctx.restore();
        },
        handleInput: function(action, phase) {
            var B = T.Board;
            var I = T.Input;
            if (phase === "down") {
                if (action === "pause_game") { switchTo("paused"); return; }
                if (!B.cur) return;
                I.keysDown[action] = true;
                if (action === "move_left") {
                    B.moveLeft(); I.das.dir = -1; I.das.timer = 0; I.das.active = false; I.das.key = action;
                } else if (action === "move_right") {
                    B.moveRight(); I.das.dir = 1; I.das.timer = 0; I.das.active = false; I.das.key = action;
                } else if (action === "soft_drop") {
                    I.softDrop.active = true; I.softDrop.timer = 0;
                    if (B.moveDown()) B.score += 1;
                } else if (action === "hard_drop") {
                    if (!B.hardDrop()) { gameOver(); return; }
                    if (B.checkModeEnd()) { finishGame(); return; }
                } else if (action === "rotate_cw") {
                    B.rotateCW();
                } else if (action === "rotate_ccw") {
                    B.rotateCCW();
                } else if (action === "hold_piece") {
                    B.doHold();
                }
            } else if (phase === "up") {
                I.keysDown[action] = false;
                if (action === "move_left" || action === "move_right") {
                    if (I.das.key === action) {
                        I.das.dir = 0; I.das.active = false;
                        if (action === "move_left" && I.keysDown["move_right"]) {
                            I.das.dir = 1; I.das.timer = 0; I.das.key = "move_right";
                        } else if (action === "move_right" && I.keysDown["move_left"]) {
                            I.das.dir = -1; I.das.timer = 0; I.das.key = "move_left";
                        }
                    }
                }
                if (action === "soft_drop") I.softDrop.active = false;
            }
        },
        keydown: function(key) {
            var action = T.Input.getActionForKey(key);
            if (action) this.handleInput(action, "down");
        },
        keyup: function(key) {
            var action = T.Input.getActionForKey(key);
            if (action) this.handleInput(action, "up");
        }
    };

    // --- PAUSED ---
    screens.paused = {
        enter: function() {
            menuIndex = 0;
            T.Audio.pauseMusic();
            showHUD();
            showOverlay("pause");
            updateSelection("pause");
        },
        exit: function() {
            hideHUD();
        },
        update: function() {},
        draw: function(ctx, W, H) {
            ctx.fillStyle = "#06060a";
            ctx.fillRect(0, 0, W, H);
            T.Board.calcLayout(W, H);
            T.Board.drawBoard(ctx);
            T.Board.drawPreviews(ctx);
        },
        keydown: function(key) {
            menuNav("pause", key, function(idx) {
                if (idx === 0) { // Resume
                    T.Audio.resumeMusic();
                    switchTo("playing");
                } else if (idx === 1) { // Settings
                    backTarget = "paused";
                    switchTo("settings");
                } else if (idx === 2) { // Restart
                    T.Audio.stopMusic();
                    T.Board.startGame(T.Board.mode);
                    switchTo("countdown");
                } else if (idx === 3) { // Quit
                    T.Audio.stopMusic();
                    switchTo("title");
                }
            }, {
                onBack: function() {
                    T.Audio.resumeMusic();
                    switchTo("playing");
                }
            });
        }
    };

    // --- GAME OVER / STATS ---
    screens.gameOver = {
        enter: function() {
            menuIndex = 0;
            T.Audio.stopMusic();
            if (!T.Board.finished) T.Audio.sfxGameOver();

            var B = T.Board;
            var isHS = false;
            if (B.mode === "sprint" && B.finished) {
                isHS = T.Storage.isHighScore("sprint", B.gameTime);
            } else {
                isHS = T.Storage.isHighScore(B.mode, B.score);
            }

            // Save high score
            if (isHS) {
                var entry = {
                    score: B.score, level: B.level, lines: B.totalLines,
                    time: B.gameTime, date: new Date().toISOString().slice(0, 10)
                };
                T.Storage.addHighScore(B.mode, entry);
            }

            // Build stats display
            var statsEl = document.getElementById("gameover-stats");
            if (statsEl) {
                var modeLabel = B.mode.charAt(0).toUpperCase() + B.mode.slice(1);
                var header = B.finished ? modeLabel + " Complete!" : "Game Over";
                var title = document.querySelector("#screen-gameover .overlay-title");
                if (title) title.textContent = header;

                var lines = [];
                lines.push("Score: " + B.score + "    Level: " + B.level);
                lines.push("Lines: " + B.totalLines + "    Pieces: " + B.piecesPlaced);
                lines.push("Time: " + B.formatTime(B.gameTime));
                lines.push("");
                lines.push("Singles: " + B.stats.singles + "  Doubles: " + B.stats.doubles);
                lines.push("Triples: " + B.stats.triples + "  Tetrises: " + B.stats.tetrises);
                lines.push("Max Combo: " + B.stats.maxCombo);
                if (isHS) lines.push("\n\u2605 NEW HIGH SCORE! \u2605");
                statsEl.textContent = lines.join("\n");
            }

            showOverlay("gameover");
            updateSelection("gameover");
        },
        exit: function() {},
        update: function() {},
        draw: function(ctx, W, H) {
            ctx.fillStyle = "#06060a";
            ctx.fillRect(0, 0, W, H);
            T.Board.calcLayout(W, H);
            T.Board.drawBoard(ctx);
        },
        keydown: function(key) {
            menuNav("gameover", key, function(idx) {
                if (idx === 0) { // Play Again
                    T.Board.startGame(T.Board.mode);
                    switchTo("countdown");
                } else if (idx === 1) { // High Scores
                    switchTo("highScores");
                } else if (idx === 2) { // Main Menu
                    switchTo("title");
                }
            });
        }
    };

    // --- HIGH SCORES ---
    screens.highScores = {
        enter: function() {
            menuIndex = 0;
            hsMode = "marathon";
            showOverlay("highscores");
            this.refreshDisplay();
            updateSelection("highscores");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        refreshDisplay: function() {
            // Update tab highlight
            var tabs = ["hs-tab-marathon", "hs-tab-sprint", "hs-tab-ultra"];
            var modes = ["marathon", "sprint", "ultra"];
            for (var i = 0; i < tabs.length; i++) {
                var el = document.getElementById(tabs[i]);
                if (el) el.className = (modes[i] === hsMode) ? "hs-tab active" : "hs-tab";
            }

            var scores = T.Storage.loadHighScores();
            var list = scores[hsMode] || [];
            var el = document.getElementById("hs-list");
            if (!el) return;

            if (list.length === 0) {
                el.textContent = "No scores yet";
                return;
            }

            var lines = [];
            for (var i = 0; i < list.length; i++) {
                var s = list[i];
                var rank = (i + 1) + ".";
                if (i < 9) rank = " " + rank;
                if (hsMode === "sprint") {
                    lines.push(rank + " " + T.Board.formatTime(s.time) + "  Lv" + s.level);
                } else {
                    lines.push(rank + " " + s.score + "  Lv" + s.level + "  " + s.lines + "L");
                }
            }
            el.textContent = lines.join("\n");
        },
        keydown: function(key) {
            var self = this;
            if (key === "ArrowLeft" || key === "ArrowRight") {
                var modes = ["marathon", "sprint", "ultra"];
                var idx = modes.indexOf(hsMode);
                if (key === "ArrowLeft") idx = (idx - 1 + 3) % 3;
                else idx = (idx + 1) % 3;
                hsMode = modes[idx];
                self.refreshDisplay();
                T.Audio.sfxMenuMove();
                return;
            }
            menuNav("highscores", key, function(idx) {
                switchTo("title");
            }, { onBack: function() { switchTo("title"); } });
        }
    };

    // --- HOW TO PLAY ---
    screens.howToPlay = {
        enter: function() {
            menuIndex = 0;
            showOverlay("howtoplay");
            // Build controls reference
            var el = document.getElementById("htp-controls");
            if (el) {
                var lines = [];
                for (var i = 0; i < T.Input.ACTIONS.length; i++) {
                    var a = T.Input.ACTIONS[i];
                    var keys = T.Input.getKeys(a.name);
                    var display = keys.length > 0 ? T.Input.keyDisplayName(keys[0]) : "???";
                    var pad = "                    ";
                    var label = a.label + pad;
                    lines.push(label.substring(0, 16) + display);
                }
                el.textContent = lines.join("\n");
            }
            updateSelection("howtoplay");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        keydown: function(key) {
            menuNav("howtoplay", key, function() {
                switchTo("title");
            }, { onBack: function() { switchTo("title"); } });
        }
    };

    // --- CREDITS ---
    screens.credits = {
        enter: function() {
            menuIndex = 0;
            showOverlay("credits");
            updateSelection("credits");
        },
        exit: function() {},
        update: function(dt, W, H) { updateBgPieces(dt, W, H); },
        draw: function(ctx, W, H) { drawBgPieces(ctx, W, H); },
        keydown: function(key) {
            menuNav("credits", key, function() {
                switchTo("title");
            }, { onBack: function() { switchTo("title"); } });
        }
    };

    // --- Internal helpers ---
    function gameOver() {
        T.Board.cur = null;
        T.Board.finished = false;
        switchTo("gameOver");
    }

    function finishGame() {
        T.Board.cur = null;
        T.Board.finished = true;
        switchTo("gameOver");
    }

    function switchTo(name) {
        if (current && current.exit) current.exit();
        currentName = name;
        current = screens[name];
        if (current && current.enter) current.enter();
    }

    // --- Public API ---
    return {
        init: function() {
            overlay = document.getElementById("overlay");
            initBgPieces();

            // Mouse support for menu items
            overlay.addEventListener("mousemove", function(e) {
                if (!activeScreenId) return;
                var target = e.target;
                while (target && target !== overlay) {
                    if (target.className && target.className.indexOf("menu-item") !== -1) break;
                    target = target.parentNode;
                }
                if (!target || target === overlay) return;
                var items = getMenuItems(activeScreenId);
                for (var i = 0; i < items.length; i++) {
                    if (items[i] === target) {
                        if (menuIndex !== i) {
                            menuIndex = i;
                            updateSelection(activeScreenId);
                            T.Audio.sfxMenuMove();
                        }
                        break;
                    }
                }
            });

            overlay.addEventListener("click", function(e) {
                if (!activeScreenId) return;
                var target = e.target;
                while (target && target !== overlay) {
                    if (target.className && target.className.indexOf("menu-item") !== -1) break;
                    target = target.parentNode;
                }
                if (!target || target === overlay) return;
                var items = getMenuItems(activeScreenId);
                for (var i = 0; i < items.length; i++) {
                    if (items[i] === target) {
                        menuIndex = i;
                        updateSelection(activeScreenId);
                        // Simulate Enter key to trigger the screen's select handler
                        if (current && current.keydown) current.keydown("Enter");
                        break;
                    }
                }
            });
        },
        switchTo: switchTo,
        getName: function() { return currentName; },
        update: function(dt, W, H) { if (current && current.update) current.update(dt, W, H); },
        draw: function(ctx, W, H) { if (current && current.draw) current.draw(ctx, W, H); },
        keydown: function(key) { if (current && current.keydown) current.keydown(key); },
        keyup: function(key) { if (current && current.keyup) current.keyup(key); },
        onAction: function(action, phase, key) {
            if (current && current.onAction) current.onAction(action, phase, key);
        }
    };
})();
