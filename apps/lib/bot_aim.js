// bot_aim.js — Shared bot aim tracker for FPS-style and top-down agents.
//
// Models a turreted gun with finite turning speed and finite reaction time:
//   - A *desired* aim (yaw + pitch) is resampled at a fixed Hz (default 15).
//     Between samples, the bot is "committed" to its last decision — this is
//     the reaction lag a human shooter has and prevents perfect frame-by-frame
//     tracking that feels machine-like.
//   - The actual aim rotates toward desired at most turnSpeed radians/sec on
//     each axis. Combined with the reaction lag, fast-strafing targets cause
//     the aim to lag behind, exactly like a real shooter swinging onto a mover.
//   - canFire() reports whether the gun is on target within fireConeRad
//     (default ~8.6°). Use this to gate shooting so the bot doesn't spray
//     while rotating.
//
// Usage:
//   <script src="../lib/bot_aim.js"></script>
//   var aim = BotAim.create({ turnSpeed: 5, sampleHz: 15, fireConeRad: 0.15 });
//   // each tick:
//   BotAim.requestAimAt(aim, simT, botX, botEyeY, botZ, tgtX, tgtY, tgtZ);
//   BotAim.tick(aim, dt);
//   if (BotAim.canFireAt(aim, botX, botEyeY, botZ, tgtX, tgtY, tgtZ)) shoot();
//
// Convention: yaw 0 faces -Z (engine standard). Forward vector =
//   (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch)).
var BotAim = {};
(function () {
    "use strict";

    function angleDelta(from, to) {
        var d = to - from;
        while (d >  Math.PI) d -= 2 * Math.PI;
        while (d < -Math.PI) d += 2 * Math.PI;
        return d;
    }

    BotAim.create = function (opts) {
        opts = opts || {};
        return {
            yaw: 0, pitch: 0,
            desiredYaw: 0, desiredPitch: 0,
            sampleT: -1e9,
            turnSpeed: opts.turnSpeed != null ? opts.turnSpeed : 5.0,
            sampleInterval: 1.0 / (opts.sampleHz != null ? opts.sampleHz : 15),
            fireConeRad: opts.fireConeRad != null ? opts.fireConeRad : 0.15,
        };
    };

    // Reset aim to a known orientation (e.g. on respawn).
    BotAim.set = function (aim, yaw, pitch) {
        aim.yaw = yaw; aim.pitch = pitch || 0;
        aim.desiredYaw = aim.yaw; aim.desiredPitch = aim.pitch;
        aim.sampleT = -1e9;
    };

    // Request a specific yaw/pitch as the new desired aim. Throttled by
    // sampleInterval — calls between samples are dropped (intentional reaction lag).
    BotAim.requestAim = function (aim, simT, yaw, pitch) {
        if (simT - aim.sampleT < aim.sampleInterval) return;
        aim.sampleT = simT;
        aim.desiredYaw = yaw;
        aim.desiredPitch = pitch || 0;
    };

    // Convenience: request aim at a 3D world point from a 3D origin.
    BotAim.requestAimAt = function (aim, simT, fromX, fromY, fromZ, toX, toY, toZ) {
        var dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
        var horizDist = Math.sqrt(dx * dx + dz * dz);
        if (horizDist < 1e-4 && Math.abs(dy) < 1e-4) return;
        // -Z forward convention: yaw 0 → -Z, so atan2(+X, -Z).
        var yaw = Math.atan2(dx, -dz);
        var pitch = Math.atan2(dy, horizDist);
        BotAim.requestAim(aim, simT, yaw, pitch);
    };

    // Per-tick rotator. Call every sim step regardless of whether requestAim
    // fired — this is what produces smooth motion between 15 Hz samples.
    BotAim.tick = function (aim, dt) {
        var maxStep = aim.turnSpeed * dt;
        var dy = angleDelta(aim.yaw, aim.desiredYaw);
        if (dy >  maxStep) dy =  maxStep;
        else if (dy < -maxStep) dy = -maxStep;
        aim.yaw += dy;
        // Wrap yaw into (-π, π] to keep numbers tidy.
        if (aim.yaw >  Math.PI) aim.yaw -= 2 * Math.PI;
        else if (aim.yaw < -Math.PI) aim.yaw += 2 * Math.PI;

        var dp = aim.desiredPitch - aim.pitch;
        if (dp >  maxStep) dp =  maxStep;
        else if (dp < -maxStep) dp = -maxStep;
        aim.pitch += dp;
        // Pitch stays in (-π/2, π/2); clamp.
        var P = Math.PI / 2 - 0.01;
        if (aim.pitch >  P) aim.pitch =  P;
        else if (aim.pitch < -P) aim.pitch = -P;
    };

    // Forward unit vector from current aim (engine convention: -Z forward).
    BotAim.forward = function (aim) {
        var cp = Math.cos(aim.pitch);
        return {
            x:  Math.sin(aim.yaw) * cp,
            y:  Math.sin(aim.pitch),
            z: -Math.cos(aim.yaw) * cp,
        };
    };

    // Is the gun aligned with a target direction within fireConeRad?
    BotAim.canFireAt = function (aim, fromX, fromY, fromZ, toX, toY, toZ) {
        var dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
        var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-4) return false;
        var f = BotAim.forward(aim);
        var dot = (f.x * dx + f.y * dy + f.z * dz) / len;
        // dot ≥ cos(coneRad) means inside the cone.
        return dot >= Math.cos(aim.fireConeRad);
    };
})();
