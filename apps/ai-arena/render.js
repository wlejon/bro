// render.js — 2D canvas drawing of arena, agents, paths, FOV, projectiles, HUDs.
var Render = {};
(function () {
    "use strict";

    var W = 700, H = 700;

    // World → screen transform. World is 40x40 centered at origin.
    function w2s(x, z) {
        var s = W / 40;
        return { x: (x + 20) * s, y: (z + 20) * s };
    }
    Render.w2s = w2s;

    Render.drawArena = function (ctx) {
        ctx.fillStyle = "#1a1d22";
        ctx.fillRect(0, 0, W, H);

        // Grid — single batched path.
        ctx.strokeStyle = "#22272e";
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var i = 0; i <= 40; i += 4) {
            var p = w2s(i - 20, -20);
            ctx.moveTo(p.x, 0); ctx.lineTo(p.x, H);
            ctx.moveTo(0, p.x); ctx.lineTo(W, p.x);
        }
        ctx.stroke();

        // Obstacles
        ctx.fillStyle = "#0a0b0d";
        ctx.strokeStyle = "#3a4048";
        for (var j = 0; j < Arena.OBSTACLES.length; j++) {
            var o = Arena.OBSTACLES[j];
            var tl = w2s(o.x - o.hw, o.z - o.hd);
            var br = w2s(o.x + o.hw, o.z + o.hd);
            ctx.fillRect(tl.x, tl.y, br.x - tl.x, br.y - tl.y);
            ctx.strokeRect(tl.x, tl.y, br.x - tl.x, br.y - tl.y);
        }
    };

    Render.drawAgents = function (ctx, agents, focusId) {
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            var u = a.unit;
            // Cache unit fields — each getter crosses QuickJS → C++.
            var uAlive = u.alive;
            var uTeam = u.teamId;
            var uId = u.id;
            var uHp = u.hp, uMaxHp = u.maxHp;
            var uMana = u.mana, uMaxMana = u.maxMana;
            var ax = a.x, az = a.z;
            var pos = w2s(ax, az);
            var color = Arena.COLORS[uTeam] || "#888";

            if (!uAlive) continue;  // dead bots don't clutter the battlefield

            var isFocus = (uId === focusId);

            // Path polyline — focused agent only.
            if (isFocus) {
                var path = a.path;
                if (path && path.length > 1) {
                    ctx.strokeStyle = color + "88";
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(pos.x, pos.y);
                    for (var k = 0; k < path.length; k++) {
                        var pp = w2s(path[k].x, path[k].z);
                        ctx.lineTo(pp.x, pp.y);
                    }
                    ctx.stroke();
                }
            }

            // FOV cone — use the aim direction we latched toward the current
            // target, not the velocity yaw (which rotates 90° when strafing
            // and made the cone visibly "face sideways" while firing).
            var FOV = Math.PI / 2.2; // ~82 deg
            var RANGE = u.attackRange || 9;
            var memForAim = (typeof AI !== "undefined") ? AI.memory[uId] : null;
            var aimX, aimZ;
            if (memForAim && memForAim.aim) {
                var f = BotAim.forward(memForAim.aim);
                aimX = f.x; aimZ = f.z;
            } else {
                aimX = Math.sin(a.yaw); aimZ = -Math.cos(a.yaw);
            }
            var aimAng = Math.atan2(aimZ, aimX);
            var p1 = w2s(ax + Math.cos(aimAng - FOV / 2) * RANGE,
                         az + Math.sin(aimAng - FOV / 2) * RANGE);
            var p2 = w2s(ax + Math.cos(aimAng + FOV / 2) * RANGE,
                         az + Math.sin(aimAng + FOV / 2) * RANGE);
            ctx.fillStyle = color + "1a";
            ctx.beginPath();
            ctx.moveTo(pos.x, pos.y);
            ctx.lineTo(p1.x, p1.y);
            ctx.lineTo(p2.x, p2.y);
            ctx.closePath();
            ctx.fill();

            // Velocity arrow (focused agent only)
            if (isFocus) {
                var v = a.velocity;
                if (v.x || v.z) {
                    var vend = w2s(ax + v.x * 0.25, az + v.z * 0.25);
                    ctx.strokeStyle = color;
                    ctx.lineWidth = 1.5;
                    ctx.beginPath();
                    ctx.moveTo(pos.x, pos.y);
                    ctx.lineTo(vend.x, vend.y);
                    ctx.stroke();
                }
            }

            // Target line — only for the focused agent, otherwise the canvas
            // fills with crisscrossing lines once there are many bots.
            var mem = (typeof AI !== "undefined") ? AI.memory[uId] : null;
            if (isFocus) {
                var tgtId = mem && mem.targetId;
                var tgt = null;
                if (tgtId != null) {
                    for (var tt = 0; tt < agents.length; tt++) {
                        if (agents[tt].unit.id === tgtId) { tgt = agents[tt]; break; }
                    }
                }
                if (tgt && tgt.unit.alive) {
                    var los = bro.ai.game.hasLineOfSight(
                        ax, az, tgt.x, tgt.z, Arena.OBSTACLES);
                    ctx.strokeStyle = los ? "rgba(80,220,120,0.7)" : "rgba(230,80,80,0.6)";
                    ctx.lineWidth = 1.5;
                    var tp = w2s(tgt.x, tgt.z);
                    ctx.beginPath();
                    ctx.moveTo(pos.x, pos.y);
                    ctx.lineTo(tp.x, tp.y);
                    ctx.stroke();
                }
            }

            // Agent body
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.arc(pos.x, pos.y, 8, 0, Math.PI * 2);
            ctx.fill();

            if (isFocus) {
                ctx.strokeStyle = "#ffd24a";
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 11, 0, Math.PI * 2);
                ctx.stroke();
            }

            // HP bar
            var hpFrac = Math.max(0, Math.min(1, uHp / uMaxHp));
            ctx.fillStyle = "#222";
            ctx.fillRect(pos.x - 12, pos.y - 16, 24, 3);
            ctx.fillStyle = hpFrac > 0.5 ? "#2ecc71" : hpFrac > 0.25 ? "#f39c12" : "#e74c3c";
            ctx.fillRect(pos.x - 12, pos.y - 16, 24 * hpFrac, 3);

            // Mana bar
            var mFrac = Math.max(0, Math.min(1, uMana / Math.max(1, uMaxMana)));
            ctx.fillStyle = "#222";
            ctx.fillRect(pos.x - 12, pos.y - 12, 24, 2);
            ctx.fillStyle = "#4a8ad4";
            ctx.fillRect(pos.x - 12, pos.y - 12, 24 * mFrac, 2);

            // Intent label — tiny text under the dot so you can watch the
            // state machine flip live (ENGAGE / KITE / FLEE / HEAL / etc).
            if (mem && mem.intent) {
                ctx.font = "10px Consolas, monospace";
                ctx.textAlign = "center";
                ctx.fillStyle = "#b8c0cc";
                ctx.fillText(mem.intent, pos.x, pos.y + 22);
                ctx.textAlign = "start";
            }
        }
    };

    Render.drawProjectiles = function (ctx, projectiles) {
        for (var i = 0; i < projectiles.length; i++) {
            var p = projectiles[i];
            var pos = w2s(p.x, p.z);
            var teamColor = Arena.COLORS[p.teamId] || "#fff";
            if (p.mode === "aoe") {
                ctx.fillStyle = "#f39c12";
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 5, 0, Math.PI * 2);
                ctx.fill();
            } else if (p.mode === "pierce") {
                ctx.strokeStyle = "#9b59b6";
                ctx.lineWidth = 2;
                var tail = w2s(p.x - p.vx * 0.07, p.z - p.vz * 0.07);
                ctx.beginPath();
                ctx.moveTo(tail.x, tail.y);
                ctx.lineTo(pos.x, pos.y);
                ctx.stroke();
            } else {
                ctx.fillStyle = teamColor;
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 3, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    };

    // Transient FX: explosion rings + floating damage numbers.
    Render.fx = {
        rings: [], // { x, z, t, maxT, r }
        floats: [], // { x, z, text, color, t }
    };

    Render.addExplosion = function (x, z, radius) {
        Render.fx.rings.push({ x: x, z: z, t: 0, maxT: 0.5, r: radius });
    };
    Render.addDamageNumber = function (x, z, amount, color) {
        Render.fx.floats.push({
            x: x, z: z, text: (amount | 0).toString(),
            color: color || "#ffd24a", t: 0,
        });
    };

    Render.drawFx = function (ctx, dt) {
        // Rings
        for (var i = Render.fx.rings.length - 1; i >= 0; i--) {
            var r = Render.fx.rings[i];
            r.t += dt;
            if (r.t >= r.maxT) { Render.fx.rings.splice(i, 1); continue; }
            var frac = r.t / r.maxT;
            var pos = w2s(r.x, r.z);
            var rr = (r.r * frac) * (W / 40);
            ctx.strokeStyle = "rgba(243,156,18," + (1 - frac).toFixed(2) + ")";
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(pos.x, pos.y, rr, 0, Math.PI * 2);
            ctx.stroke();
        }
        // Floating damage numbers
        ctx.font = "bold 12px Consolas, monospace";
        ctx.textAlign = "center";
        for (var j = Render.fx.floats.length - 1; j >= 0; j--) {
            var f = Render.fx.floats[j];
            f.t += dt;
            if (f.t >= 1.0) { Render.fx.floats.splice(j, 1); continue; }
            var pos2 = w2s(f.x, f.z);
            var alpha = 1 - f.t;
            ctx.fillStyle = f.color;
            ctx.globalAlpha = alpha;
            ctx.fillText(f.text, pos2.x, pos2.y - 18 - f.t * 20);
            ctx.globalAlpha = 1;
        }
        ctx.textAlign = "start";
    };

    // Replay-mode rendering from a ReplayReader frame object.
    Render.drawReplayFrame = function (ctx, frame, focusId) {
        if (!frame) return;
        var agents = frame.agents;
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            var pos = w2s(a.x, a.z);
            var team = 0;
            for (var k = 0; k < Arena.ROSTER.length; k++) {
                if (Arena.ROSTER[k].id === a.id) { team = Arena.ROSTER[k].teamId; break; }
            }
            var color = Arena.COLORS[team];
            if (!a.alive) {
                ctx.fillStyle = "#2a2f36";
            } else {
                ctx.fillStyle = color;
            }
            ctx.beginPath();
            ctx.arc(pos.x, pos.y, 8, 0, Math.PI * 2);
            ctx.fill();
            if (a.id === focusId) {
                ctx.strokeStyle = "#ffd24a";
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 11, 0, Math.PI * 2);
                ctx.stroke();
            }
            var hpFrac = Math.max(0, Math.min(1, a.hp / 100));
            ctx.fillStyle = "#222";
            ctx.fillRect(pos.x - 12, pos.y - 16, 24, 3);
            ctx.fillStyle = "#2ecc71";
            ctx.fillRect(pos.x - 12, pos.y - 16, 24 * hpFrac, 3);
        }
    };
})();
