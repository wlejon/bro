/**
 * =============================================================================
 * Gamepad API — W3C Controller Input and Dual-Motor Rumble Haptics
 * =============================================================================
 *
 * Implements the W3C Gamepad API for polling game controller hardware,
 * button/axis states, standard mappings, and dual-rumble / trigger-rumble haptic actuators.
 *
 * @example
 *   // Polling gamepad in input loop
 *   const gp = navigator.getGamepads()[0];
 *   if (gp) {
 *     const moveX = gp.axes[0];
 *     const jump = gp.buttons[0].pressed;
 *     const boost = gp.buttons[7].value;
 *     console.log("Gamepad:", gp.id, "jump:", jump, "moveX:", moveX, "boost:", boost);
 *   }
 *
 * @example
 *   // Playing rumble haptic effect
 *   const gp = navigator.getGamepads()[0];
 *   if (gp && gp.vibrationActuator) {
 *     gp.vibrationActuator.playEffect("dual-rumble", {
 *       duration: 200,
 *       strongMagnitude: 1.0,
 *       weakMagnitude: 0.4
 *     });
 *   }
 *
 * @example
 *   // Trigger-rumble haptic effect
 *   const gp = navigator.getGamepads()[0];
 *   if (gp && gp.vibrationActuator) {
 *     gp.vibrationActuator.playEffect("trigger-rumble", {
 *       duration: 120,
 *       strongMagnitude: 0.2,
 *       weakMagnitude: 0.2,
 *       leftTrigger: 0.0,
 *       rightTrigger: 1.0
 *     });
 *   }
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Parameters for playing vibration haptic effects on a Gamepad.
 * @typedef {Object} GamepadEffectParameters
 * @property {number} [duration=0] -  Duration of effect in milliseconds
 * @property {number} [strongMagnitude=0] -  Strong motor magnitude [0.0 - 1.0]
 * @property {number} [weakMagnitude=0] -  Weak motor magnitude [0.0 - 1.0]
 * @property {number} [leftTrigger=0] -  Left trigger motor magnitude [0.0 - 1.0] (trigger-rumble only)
 * @property {number} [rightTrigger=0] -  Right trigger motor magnitude [0.0 - 1.0] (trigger-rumble only)
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Represents a hardware vibration actuator attached to a Gamepad.
 */
class GamepadHapticActuator {

  /**
   *  Type of actuator (e.g. "dual-rumble")
   * @readonly
   * @type {string}
   */
  type;

  /**
   *  Supported effect types (e.g. ["dual-rumble", "trigger-rumble"])
   * @readonly
   * @type {Array<string>}
   */
  effects;

  /**
   * Plays a haptic effect on the gamepad motors.
   *
   * @param {string} type - Effect type ("dual-rumble" or "trigger-rumble")
   * @param {GamepadEffectParameters} [params] - Vibration parameters
   * @returns {Promise<string>} Promise resolving to "complete" or "preempted"
   */
  playEffect(type, params) {}

  /**
   * Resets and stops any active vibration effect.
   * @returns {Promise<string>} Promise resolving to "complete"
   */
  reset() {}

}

/**
 * An individual button on a Gamepad device.
 */
class GamepadButton {

  /**
   *  Whether the button is currently pressed past activation threshold
   * @readonly
   * @type {boolean}
   */
  pressed;

  /**
   *  Whether the button is touched or analog value > 0
   * @readonly
   * @type {boolean}
   */
  touched;

  /**
   *  Current analog displacement value [0.0 - 1.0]
   * @readonly
   * @type {number}
   */
  value;

}

/**
 * Represents a gamepad/controller connected to the system.
 */
class Gamepad {

  /**
   *  Identifier string of the gamepad device
   * @readonly
   * @type {string}
   */
  id;

  /**
   *  Unique zero-based slot index
   * @readonly
   * @type {number}
   */
  index;

  /**
   *  Whether the gamepad is connected
   * @readonly
   * @type {boolean}
   */
  connected;

  /**
   *  Layout mapping ("standard")
   * @readonly
   * @type {string}
   */
  mapping;

  /**
   *  17 button snapshot states
   * @readonly
   * @type {Array<GamepadButton>}
   */
  buttons;

  /**
   *  4 stick axis values [-1.0 - 1.0]
   * @readonly
   * @type {Array<number>}
   */
  axes;

  /**
   *  Timestamp in ms when the state changed
   * @readonly
   * @type {number}
   */
  timestamp;

  /**
   *  Vibration haptic actuator handle
   * @readonly
   * @type {GamepadHapticActuator}
   */
  vibrationActuator;

}

/**
 * Event fired when a Gamepad is connected or disconnected.
 */
class GamepadEvent {

  /**
   *  The gamepad snapshot associated with this event
   * @readonly
   * @type {Gamepad}
   */
  gamepad;

}

