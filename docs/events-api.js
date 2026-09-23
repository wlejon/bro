/**
 * =============================================================================
 * Pointer Events, Touch Events & DOM Event System
 * =============================================================================
 *
 * W3C Pointer Events and Touch Events unified input model over mouse and touch.
 * Includes Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, Touch, TouchList,
 * TouchEvent, and GestureEvent interfaces.
 *
 * @example
 *   // Unified pointer handling
 *   canvas.addEventListener('pointerdown', (e) => {
 *     canvas.setPointerCapture(e.pointerId);
 *     console.log('Pointer down:', e.pointerType, e.pointerId, e.clientX, e.clientY);
 *   });
 *
 * @example
 *   // Two-finger pinch gesture
 *   el.addEventListener('gesturechange', (e) => {
 *     console.log('Scale:', e.scale, 'Rotation:', e.rotation);
 *   });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Options for initializing an Event.
 * @typedef {Object} EventInit
 * @property {boolean} [bubbles=false]
 * @property {boolean} [cancelable=false]
 * @property {boolean} [composed=false]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Root interface for DOM event dispatching and propagation.
 */
class Event {

  /**
   *  Initializes a new Event instance.
   *
   * @param {string} type
   * @param {EventInit} [eventInitDict]
   */
  constructor(type, eventInitDict) {}

  /**
   *  Type of event.
   * @readonly
   * @type {string}
   */
  type;

  /**
   *  Event target element.
   * @readonly
   * @type {Object|null}
   */
  target;

  /**
   *  Current event target element.
   * @readonly
   * @type {Object|null}
   */
  currentTarget;

  /**
   *  Whether event bubbles up the DOM tree.
   * @readonly
   * @type {boolean}
   */
  bubbles;

  /**
   *  Whether event default action can be prevented.
   * @readonly
   * @type {boolean}
   */
  cancelable;

  /**
   *  Whether preventDefault was called on this event.
   * @readonly
   * @type {boolean}
   */
  defaultPrevented;

  /**
   *  Creation timestamp of event in milliseconds.
   * @readonly
   * @type {number}
   */
  timeStamp;

  /**
   *  Prevents default browser/engine action for this event.
   */
  preventDefault() {}

  /**
   *  Stops further event propagation in the DOM tree.
   */
  stopPropagation() {}

  /**
   *  Deprecated legacy event initializer.
   *
   * @param {string} type
   * @param {boolean} bubbles
   * @param {boolean} cancelable
   */
  initEvent(type, bubbles, cancelable) {}

}

/**
 * Event carrying custom user data in the detail property.
 */
class CustomEvent extends Event {

  /**
   *  User-supplied detail payload.
   * @readonly
   * @type {*}
   */
  detail;

}

/**
 * Base interface for UI and user-input events.
 */
class UIEvent extends Event {

  /**
   *  Window view context.
   * @readonly
   * @type {*}
   */
  view;

  /**
   *  Numeric event detail.
   * @readonly
   * @type {number}
   */
  detail;

}

/**
 * Mouse button and cursor movement event.
 */
class MouseEvent extends UIEvent {

  /**
   * @readonly
   * @type {number}
   */
  screenX;

  /**
   * @readonly
   * @type {number}
   */
  screenY;

  /**
   * @readonly
   * @type {number}
   */
  clientX;

  /**
   * @readonly
   * @type {number}
   */
  clientY;

  /**
   * @readonly
   * @type {number}
   */
  offsetX;

  /**
   * @readonly
   * @type {number}
   */
  offsetY;

  /**
   * @readonly
   * @type {number}
   */
  pageX;

  /**
   * @readonly
   * @type {number}
   */
  pageY;

  /**
   * @readonly
   * @type {boolean}
   */
  ctrlKey;

  /**
   * @readonly
   * @type {boolean}
   */
  shiftKey;

  /**
   * @readonly
   * @type {boolean}
   */
  altKey;

  /**
   * @readonly
   * @type {boolean}
   */
  metaKey;

  /**
   * @readonly
   * @type {number}
   */
  button;

  /**
   * @readonly
   * @type {number}
   */
  buttons;

  /**
   * @readonly
   * @type {Object|null}
   */
  relatedTarget;

}

/**
 * Unified pointer event for mouse, pen, and touch input.
 */
class PointerEvent extends MouseEvent {

  /**
   *  Unique identifier for the pointer device/contact.
   * @readonly
   * @type {number}
   */
  pointerId;

  /**
   *  Device type ('mouse' | 'touch' | 'pen').
   * @readonly
   * @type {string}
   */
  pointerType;

  /**
   *  Whether this contact is the primary pointer.
   * @readonly
   * @type {boolean}
   */
  isPrimary;

  /**
   *  Contact pressure in range [0, 1].
   * @readonly
   * @type {number}
   */
  pressure;

  /**
   *  Contact width in pixels.
   * @readonly
   * @type {number}
   */
  width;

  /**
   *  Contact height in pixels.
   * @readonly
   * @type {number}
   */
  height;

}

/**
 * Individual touch contact point on a touch-sensitive surface.
 */
class Touch {

  /**
   * @readonly
   * @type {number}
   */
  identifier;

  /**
   * @readonly
   * @type {Object}
   */
  target;

  /**
   * @readonly
   * @type {number}
   */
  screenX;

  /**
   * @readonly
   * @type {number}
   */
  screenY;

  /**
   * @readonly
   * @type {number}
   */
  clientX;

  /**
   * @readonly
   * @type {number}
   */
  clientY;

  /**
   * @readonly
   * @type {number}
   */
  pageX;

  /**
   * @readonly
   * @type {number}
   */
  pageY;

  /**
   * @readonly
   * @type {number}
   */
  force;

}

/**
 * List of active touch contact points.
 */
class TouchList {

  /**
   * @readonly
   * @type {number}
   */
  length;

  /**
   * @param {number} index
   * @returns {Touch|null}
   */
  item(index) {}

}

/**
 * Multi-touch interaction event.
 */
class TouchEvent extends UIEvent {

  /**
   * @readonly
   * @type {TouchList}
   */
  touches;

  /**
   * @readonly
   * @type {TouchList}
   */
  targetTouches;

  /**
   * @readonly
   * @type {TouchList}
   */
  changedTouches;

  /**
   * @readonly
   * @type {boolean}
   */
  ctrlKey;

  /**
   * @readonly
   * @type {boolean}
   */
  shiftKey;

  /**
   * @readonly
   * @type {boolean}
   */
  altKey;

  /**
   * @readonly
   * @type {boolean}
   */
  metaKey;

}

/**
 * Multi-finger gesture event (pinch / pan / rotate).
 */
class GestureEvent extends UIEvent {

  /**
   *  Gesture scale relative to start.
   * @readonly
   * @type {number}
   */
  scale;

  /**
   *  Rotation in degrees clockwise since start.
   * @readonly
   * @type {number}
   */
  rotation;

  /**
   *  Centroid X in viewport coordinates.
   * @readonly
   * @type {number}
   */
  clientX;

  /**
   *  Centroid Y in viewport coordinates.
   * @readonly
   * @type {number}
   */
  clientY;

}

// ── Uncaught errors: window.onerror and the 'error' event ────────────────────
//
// An exception nothing caught — thrown out of a setTimeout/setInterval
// callback, a requestAnimationFrame callback, an event listener, a
// queueMicrotask callback, or a script's top level — is reported to the page
// the way a browser reports it:
//   1. window.onerror(message, filename, lineno, colno, error) is called
//      (legacy five-argument form); returning true cancels the report;
//   2. an ErrorEvent (cancelable) is dispatched at the window's 'error'
//      listeners; preventDefault() cancels the report.
// A cancelled report skips the engine's own `[bronze:...] uncaught` log line.
// message is 'Uncaught Name: message' (or 'Uncaught <value>' for a thrown
// primitive); bronze records no source positions, so filename is '' and
// lineno/colno are 0. An error thrown by an error handler is logged, never
// re-dispatched. A script whose top level threw is still reported as failed
// (headless exits non-zero) whatever its handlers do.
//
// NOT raised: 'unhandledrejection' / window.onunhandledrejection. bronze
// reports a promise nothing handled from inside its own microtask drain
// (stderr: "Unhandled promise rejection: ..."), with no embedder hook yet.

window.onerror = (message, filename, lineno, colno, error) => {
  report(error);
  return true;              // handled: no engine log line
};
window.addEventListener('error', (e) => {
  // e instanceof ErrorEvent; e.message, e.filename, e.lineno, e.colno, e.error
  e.preventDefault();       // handled
});

// ── window.postMessage ───────────────────────────────────────────────────────
//
// window.postMessage(message, targetOrigin[, transfer])
// window.postMessage(message, { targetOrigin = '/', transfer })
//
// Delivers a MessageEvent ('message') to the window itself, as a TASK: after
// the posting code and its microtasks have run. The message is
// structured-cloned (structuredClone, as MessageChannel ports do) at the call,
// so later mutation does not reach the receiver and an uncloneable value
// throws there. window.onmessage runs first, then 'message' listeners; the
// event carries data, origin (location.origin, 'bro://app'), source (window)
// and ports (the MessagePorts in the transfer list).
// targetOrigin: '*' always delivers; '/' means the page's own origin; an
// absolute URL delivers only when its origin is 'bro://app' (otherwise the
// message is silently dropped); anything that is not a URL throws a
// SyntaxError DOMException. A secondary window (bro.window.open) posts to its
// opener with bro.window.parent.postMessage instead.

window.addEventListener('message', (e) => { e.data; e.origin; e.source; e.ports; });
window.postMessage({ kind: 'ping' }, '*');

// ── Navigation / form event classes ──────────────────────────────────────────
//
// Constructible for dispatching and for code that sniffs them. bro has no
// history navigation, so nothing fires hashchange / popstate on its own.

new HashChangeEvent('hashchange', { oldURL: '', newURL: '' });   // oldURL, newURL
new PopStateEvent('popstate', { state: null });                  // state, hasUAVisualTransition
new FormDataEvent('formdata', { formData: new FormData() });     // formData

