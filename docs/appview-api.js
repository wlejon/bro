/**
 * =============================================================================
 * HTMLAppViewElement (<appview>) — Another App, Running Inline
 * =============================================================================
 *
 * `<appview src="dir">` runs the folder app at `dir` inside this page, the way
 * helm runs its apps. The page hosts it as it hosts an `<iframe>` (its own
 * layer, input routed in, `width` / `height` / `reload()` / `capture()`), but
 * it is ANOTHER APP, a compartment of its own (docs/app-isolation.md):
 *
 *   - It is who its `bro.json` says. `bro.app` inside it is its own (id, name,
 *     folders), and its grants are its own, settled from its manifest and the
 *     grant store, never this page's: the page's `files` does not pass to it.
 *   - Nothing mutable crosses. The page reaches it by message
 *     (`contentWindow` is a window that carries `postMessage` and `closed`
 *     only; `contentDocument` is null), and the app reaches the page the same
 *     way (`parent`, `top`). Its DOM nodes, canvases and other host objects
 *     are refused by this page's natives, and the page's by its.
 *   - The page may grant it functions: `expose(name, fn)` appears inside as
 *     `bro.app.host[name]`. A call clones its arguments into the page, runs
 *     `fn` there and clones the result back (a promise settles a promise of
 *     the app's). A function passed as an argument becomes a capability the
 *     other way.
 *   - Its permission prompts are the page's to draw: an undecided capability
 *     is a `permissionrequest` event at the element (below). Unhandled, bro's
 *     own dialog asks.
 *   - It cannot stall the page. It runs in time slices (8 ms) on a stack of
 *     its own: a timer, frame callback, message or script that runs longer
 *     yields and finishes over later frames, while the page goes on. Where
 *     the page waits on it (an event dispatched into it, a function it handed
 *     the page), it must finish within a second, or bro stops it and fires
 *     `terminated` at the element with the reason; `start()` runs it again.
 *   - What it held is let go when it stops (`stop()`, termination, removal):
 *     its timers, Workers, sound, watchers and child processes
 *     (`<terminal>`, `child_process`).
 *
 * Not yet in an app's view, as in any sub-document: `getContext('webgl2')`
 * and `getContext('scene')` answer null.
 *
 * An `<iframe>` of the page's own folder is the page's own app, and reaches
 * and is reached as a same-origin frame is on the web (docs/iframe-api.js).
 *
 * @example
 *   const view = document.createElement('appview');
 *   view.expose('notify', (text) => showToast(text));
 *   view.addEventListener('permissionrequest', (e) => {
 *     e.preventDefault();               // the page asks in its own time
 *     askUser(e.detail).then((yes) => (yes ? e.allow() : e.deny()));
 *   });
 *   view.src = '../music';
 *   document.body.appendChild(view);
 *   // Inside the app: bro.app.host.notify('now playing');
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * An app running inline in this page.
 */
class HTMLAppViewElement extends HTMLIFrameElement {

  /**
   *  The app's folder. Setting it loads that app in place of the running one.
   * @type {string}
   */
  src;

  /**
   *  The app's window as this page may reach it: `postMessage(message,
   *  targetOrigin?, transfer?)` and `closed`; null while it is not running.
   * @readonly
   * @type {{postMessage: function(*, string=, Array=): void, closed: boolean}|null}
   */
  contentWindow;

  /**
   *  Always null: another app's document is not this page's to reach.
   * @readonly
   * @type {null}
   */
  contentDocument;

  /**
   *  Whether the app is running (loaded and not stopped).
   * @readonly
   * @type {boolean}
   */
  running;

  /**
   *  The app's id (its manifest's, else its canonical folder's); null while
   *  it is not running.
   * @readonly
   * @type {string|null}
   */
  appId;

  /**
   *  The app's name; null while it is not running.
   * @readonly
   * @type {string|null}
   */
  appName;

  /**
   *  What the app declares and holds, or null while it is not running.
   * @readonly
   * @type {{declared: string[], granted: string[], denied: string[], undecided: string[]}|null}
   */
  permissions;

  /**
   *  Starts the app after `stop()`.
   */
  start() {}

  /**
   *  Stops the app: what it holds open is closed, its realm goes, and its
   *  window reads `closed`. What the page exposed stays for its next start.
   */
  stop() {}

  /**
   *  Records the user's answer about one of the app's capabilities, from a
   *  prompt the page drew itself, as bro's own dialog's answer is recorded.
   * @param {string} capability e.g. 'files', 'net', 'sys.audio'
   * @param {boolean} allow
   */
  decide(capability, allow) {}

  /**
   *  Grants the app a function of this page's, as `bro.app.host[name]`
   *  inside it, called across the membrane. Exposing a name again replaces
   *  the function. Kept across `stop()` / `start()` and `reload()`.
   * @param {string} name
   * @param {Function} fn
   */
  expose(name, fn) {}
}

/**
 * Dispatched at the `<appview>` (in this page) when bro stopped the app: it
 * did not return within a second where the page waits on it, or its stack
 * ended on a host error. The view is stopped (`running` false); `start()`
 * runs the app again.
 *
 * @example
 *   view.addEventListener('terminated', (e) => {
 *     showErrorCard(`${view.appName} was stopped: ${e.detail.reason}`, e.detail.site);
 *   });
 */
class AppViewTerminatedEvent extends CustomEvent {
  /**
   * `site`: where the app was when it was stopped, innermost frame first (one
   * per line), or "".
   * @type {{reason: string, site: string}}
   */
  detail;
}

/**
 * Dispatched at the `<appview>` (in this page) when the app uses a
 * capability it declared and the user has not decided. Answered in the
 * listener with `allow()` or `deny()`, that is the user's answer. With
 * `preventDefault()` and no answer, the page asks in its own time: the use is
 * refused meanwhile, and a later `allow()` / `deny()` is recorded. With no
 * listener answering, bro's own dialog asks.
 */
class AppViewPermissionRequestEvent extends CustomEvent {
  /**
   * @type {{capability: string, question: string, what: string, app: string}}
   */
  detail;

  /** The user allows it. */
  allow() {}

  /** The user refuses it. */
  deny() {}
}
