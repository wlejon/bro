/**
 * Gamepad API — W3C-style controller input over SDL3's gamepad abstraction
 *
 * bro implements the web Gamepad API poll model: controllers surface through
 * `navigator.getGamepads()` as immutable per-call snapshots, and connection
 * changes fire `gamepadconnected` / `gamepaddisconnected` on `window`. SDL3
 * normalizes every recognized device to one logical layout, so `mapping` is
 * always `"standard"` — button and axis indices mean the same thing on every
 * controller:
 *
 *   buttons[0]  south (A / Cross)         buttons[9]   start
 *   buttons[1]  east (B / Circle)         buttons[10]  left stick click
 *   buttons[2]  west (X / Square)         buttons[11]  right stick click
 *   buttons[3]  north (Y / Triangle)      buttons[12]  dpad up
 *   buttons[4]  left shoulder (LB)        buttons[13]  dpad down
 *   buttons[5]  right shoulder (RB)       buttons[14]  dpad left
 *   buttons[6]  left trigger (LT, analog) buttons[15]  dpad right
 *   buttons[7]  right trigger (RT, analog)buttons[16]  guide
 *   buttons[8]  back / select
 *
 *   axes[0] left stick X    axes[1] left stick Y     (-1..1, down/right = +)
 *   axes[2] right stick X   axes[3] right stick Y
 *
 * Triggers are analog: `buttons[6].value` runs 0..1 and `pressed` flips at
 * 0.1. Multiple controllers get stable slot indices; a disconnected slot
 * reads `null` in getGamepads() and is reused by the next device.
 *
 * Gamepad buttons also participate in the bro.settings action-binding system
 * via `"gamepad:<name>"` binding strings — see docs/settings.md.
 *
 * Headless testing: inject a virtual controller at the engine layer and drive
 * this whole surface without hardware — see docs/headless.md.
 *
 *   gamepadConnect([id])                         -> slot index
 *   gamepadDisconnect(index)
 *   gamepadButton(index, button, pressed [, value])
 *   gamepadAxis(index, axis, value)
 *
 * `button` and `axis` take either the W3C index or the SDL name ("south",
 * "lefttrigger", "leftx", ...). `value` on gamepadButton gives triggers an
 * analog level, defaulting to pressed ? 1 : 0. The three mutators throw
 * TypeError ("no virtual gamepad at index N") if that slot is empty or holds
 * a REAL controller — they only drive pads created by gamepadConnect().
 */

// ── Polling ───────────────────────────────────────────────────────────────────

/**
 * Snapshot all controller slots.
 *
 * @returns {(Gamepad|null)[]} One entry per slot ever seen this session;
 *   `null` where the slot's device is disconnected. Empty array when no
 *   controller was ever connected. Each Gamepad is a plain snapshot — poll
 *   again for fresh state; old snapshots never mutate.
 */
navigator.getGamepads();

/**
 * A Gamepad snapshot.
 *
 * @typedef {Object} Gamepad
 * @property {string}  id        - Device name (e.g. "Xbox Series X Controller")
 * @property {number}  index     - Slot index, stable for the device's lifetime
 * @property {boolean} connected - False only on the snapshot carried by a
 *                                 gamepaddisconnected event
 * @property {"standard"} mapping - Always the W3C standard layout
 * @property {{pressed: boolean, touched: boolean, value: number}[]} buttons
 *                                 - 17 entries; value is 0..1 (analog for the
 *                                 triggers, 0/1 for digital buttons).
 *                                 `touched` is derived, not sensed: it is
 *                                 `pressed || value > 0`, so a trigger held
 *                                 below the 0.1 press threshold reads
 *                                 touched:true with pressed:false.
 * @property {number[]} axes     - 4 stick axes, each -1..1
 * @property {number}  timestamp - Engine wall-clock ms of the last state change
 * @property {GamepadHapticActuator} vibrationActuator - Rumble control.
 *   `effects` lists ["dual-rumble", "trigger-rumble"].
 *   playEffect("dual-rumble", {duration, strongMagnitude, weakMagnitude})
 *   drives the body motors; playEffect("trigger-rumble", {duration,
 *   strongMagnitude, weakMagnitude, leftTrigger, rightTrigger}) additionally
 *   drives the per-trigger motors (Xbox-style pads; others ignore the
 *   trigger part). reset() stops both.
 */

// Typical read-in-game-loop usage:
function pollInput() {
    const gp = navigator.getGamepads()[0];
    if (!gp) return;                       // nothing in slot 0
    const moveX = gp.axes[0];              // left stick
    const jump  = gp.buttons[0].pressed;   // south button
    const boost = gp.buttons[7].value;     // right trigger, analog 0..1
}

// ── Connection events ─────────────────────────────────────────────────────────

/**
 * Fired on window when a controller arrives / leaves. `event.gamepad` is a
 * snapshot taken at the moment of the event (connected=false on disconnect).
 */
window.addEventListener("gamepadconnected", (e) => {
    console.log("pad", e.gamepad.index, e.gamepad.id);
});
window.addEventListener("gamepaddisconnected", (e) => {
    console.log("lost pad", e.gamepad.index);
});

// ── Rumble ────────────────────────────────────────────────────────────────────

/**
 * Rumble haptics, mapped to SDL_RumbleGamepad (+ SDL_RumbleGamepadTriggers).
 *
 * @typedef {Object} GamepadHapticActuator
 * @property {"dual-rumble"} type
 * @property {("dual-rumble"|"trigger-rumble")[]} effects
 *   - Always ["dual-rumble", "trigger-rumble"]
 */

/**
 * Start a rumble effect. `"dual-rumble"` and `"trigger-rumble"` are both
 * accepted; any other type throws TypeError. startDelay is ignored (the
 * effect starts immediately).
 *
 * "trigger-rumble" drives the body motors exactly like "dual-rumble" AND the
 * per-trigger motors from leftTrigger / rightTrigger (Xbox-style pads; others
 * ignore the trigger part).
 *
 * @param {"dual-rumble"|"trigger-rumble"} type
 * @param {Object} [params]
 * @param {number} [params.duration=0]        - Effect length in ms
 * @param {number} [params.strongMagnitude=0] - Low-frequency motor, 0..1
 * @param {number} [params.weakMagnitude=0]   - High-frequency motor, 0..1
 * @param {number} [params.leftTrigger=0]     - Left trigger motor, 0..1
 *                                              ("trigger-rumble" only)
 * @param {number} [params.rightTrigger=0]    - Right trigger motor, 0..1
 *                                              ("trigger-rumble" only)
 * @returns {Promise<"complete"|"preempted">} Resolves immediately ("preempted"
 *   if the slot is gone); it does not wait out the duration.
 */
gamepad.vibrationActuator.playEffect("dual-rumble", {
    duration: 200, strongMagnitude: 1.0, weakMagnitude: 0.4,
});

gamepad.vibrationActuator.playEffect("trigger-rumble", {
    duration: 120, strongMagnitude: 0.2, weakMagnitude: 0.2,
    leftTrigger: 0.0, rightTrigger: 1.0,     // recoil in the right trigger
});

/**
 * Stop any in-progress rumble — both the body and the trigger motors.
 * @returns {Promise<"complete">}
 */
gamepad.vibrationActuator.reset();

// ── Action binding (bro.settings integration) ─────────────────────────────────

/**
 * Gamepad buttons bind to named actions exactly like keyboard keys, using
 * "gamepad:<name>" strings. Names (matching SDL's mapping-string fields):
 *   south east west north leftshoulder rightshoulder lefttrigger righttrigger
 *   back start leftstick rightstick dpup dpdown dpleft dpright guide
 *
 * Stick-axis DIRECTIONS bind too, as "gamepad:<axis>+" / "gamepad:<axis>-"
 * (axes: leftx lefty rightx righty). An axis binding presses once the
 * deflection along its direction crosses the action's deadzone (default 0.1;
 * per-action override via defineAction's options.deadzone) and releases with
 * hysteresis below deadzone * 0.75, so jitter at the threshold can't spam
 * down/up pairs.
 *
 * Press/release edges dispatch the same "action" CustomEvent on document.body
 * as keys do (triggers count as pressed past 0.1). detail.gamepad carries the
 * slot index, detail.strength the analog contribution at the edge (trigger
 * value, deadzone-rescaled axis deflection, 1/0 for digital buttons). See
 * docs/settings.md for the full action system.
 */
bro.settings.defineAction("jump", [" ", "gamepad:south"]);
bro.settings.defineAction("pause", ["Escape", "gamepad:start"]);
bro.settings.defineAction("move_right", ["d", "gamepad:leftx+"], { deadzone: 0.25 });

document.body.addEventListener("action", (e) => {
    if (e.detail.action === "jump" && e.detail.phase === "down") {
        player.jump();   // e.detail.key === "gamepad:south" when pad-driven
    }
});

/**
 * Polled action state — per-frame analog reads without listening for edges.
 * getActionStrength(name): 0..1, max over the action's bindings (keys/mouse
 * 0/1, triggers analog, axis bindings deadzone-rescaled: (m - dz) / (1 - dz)).
 * isActionPressed(name): boolean; axis bindings answer from the same
 * hysteresis latch that drives their events. Headless-injected gamepadAxis()
 * values flow through identically, so tests can assert exact strengths.
 */
const v = bro.settings.getActionStrength("move_right");   // e.g. 0.5 at 55% stick
const held = bro.settings.isActionPressed("jump");
