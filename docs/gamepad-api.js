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
 * Headless testing: `gamepadConnect()` / `gamepadDisconnect()` /
 * `gamepadButton()` / `gamepadAxis()` inject a virtual controller at the
 * engine layer, driving this whole surface without hardware — see
 * docs/headless.md.
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
 *                                 triggers, 0/1 for digital buttons)
 * @property {number[]} axes     - 4 stick axes, each -1..1
 * @property {number}  timestamp - Engine wall-clock ms of the last state change
 * @property {GamepadHapticActuator} vibrationActuator - Dual-rumble control
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
 * Dual-rumble haptics, mapped to SDL_RumbleGamepad.
 *
 * @typedef {Object} GamepadHapticActuator
 * @property {"dual-rumble"} type
 */

/**
 * Start a rumble effect. Only `"dual-rumble"` is supported; startDelay is
 * ignored (the effect starts immediately).
 *
 * @param {"dual-rumble"} type
 * @param {Object} [params]
 * @param {number} [params.duration=0]        - Effect length in ms
 * @param {number} [params.strongMagnitude=0] - Low-frequency motor, 0..1
 * @param {number} [params.weakMagnitude=0]   - High-frequency motor, 0..1
 * @returns {Promise<"complete"|"preempted">} Resolves immediately ("preempted"
 *   if the slot is gone); it does not wait out the duration.
 */
gamepad.vibrationActuator.playEffect("dual-rumble", {
    duration: 200, strongMagnitude: 1.0, weakMagnitude: 0.4,
});

/**
 * Stop any in-progress rumble.
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
 * Press/release edges dispatch the same "action" CustomEvent on document.body
 * as keys do (triggers count as pressed past 0.1). detail.gamepad carries the
 * slot index. See docs/settings.md for the full action system.
 */
bro.settings.defineAction("jump", [" ", "gamepad:south"]);
bro.settings.defineAction("pause", ["Escape", "gamepad:start"]);

document.body.addEventListener("action", (e) => {
    if (e.detail.action === "jump" && e.detail.phase === "down") {
        player.jump();   // e.detail.key === "gamepad:south" when pad-driven
    }
});
