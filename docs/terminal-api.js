/**
 * =============================================================================
 * <terminal> — a native terminal element (HTMLTerminalElement, bro.terminal)
 * =============================================================================
 *
 * bro paints the terminal itself; script only drives it. The emulator is
 * bropty (VT/xterm, kitty keyboard protocol, synchronized output, wide and
 * combining characters, truecolor), the process runs on a PTY (ConPTY on
 * Windows), and the screen is drawn through bro's own text path in the
 * element's CSS font. Built with BRO_WITH_TERMINAL (on in the `app` and `full`
 * profiles); without it `bro.terminal.available` is false and the element is
 * an inert box.
 *
 * Layout: a replaced element, `display: block` in a monospace font by
 * default. Its intrinsic size is `cols` x `rows` cells (attributes, default
 * 80 x 24); CSS width/height override it, and the grid then follows the
 * content box: a box that changes size changes cols/rows, resizes the PTY and
 * fires `resize`.
 *
 * Input: the element is focusable (tabIndex 0, click or focus()). While it is
 * focused it takes the keyboard the way a terminal window does: Tab, Ctrl+C,
 * Ctrl+V, the arrows and function keys go to the program, encoded for the
 * modes it set (legacy xterm, modifyOtherKeys, the kitty protocol). The page
 * still gets every keydown first, and preventDefault() keeps a key from the
 * program. Copy is Ctrl+Shift+C / Ctrl+Insert, paste Ctrl+Shift+V /
 * Shift+Insert (Cmd+C / Cmd+V on macOS); a paste is bracketed when the
 * program set mode 2004. IME compositions are drawn at the cursor and only
 * the committed text is sent. Focus changes are reported under mode 1004.
 *
 * Output never runs on the main thread: a parser thread per terminal reads
 * the PTY and publishes immutable frames, at most one per frame drawn, so a
 * flood of output costs the page nothing and nothing is dropped (the child
 * is throttled instead). Synchronized output (mode 2026) holds presentation
 * until the update ends, or 200 ms.
 *
 * @example
 *   const term = document.createElement('terminal');
 *   term.style.cssText = 'width: 100%; height: 400px; font: 14px "Cascadia Mono", monospace';
 *   document.body.appendChild(term);
 *   term.addEventListener('exit', (e) => console.log('exited with', e.detail.exitCode));
 *   term.spawn({});              // the default shell
 *   term.focus();
 *
 * @example
 *   // A program, its arguments, a working directory and extra environment.
 *   term.spawn({ command: 'git', args: ['log', '--oneline'], cwd: bro.appDir,
 *                env: { GIT_PAGER: 'cat' } });
 */

// ── bro.terminal ────────────────────────────────────────────────────────────

const terminal = {
  /** Whether <terminal> is compiled into this build. @type {boolean} */
  available: true,
  /** What spawn() runs without a command: %COMSPEC% (else cmd.exe) on Windows,
   *  $SHELL (else /bin/sh) elsewhere. @type {string} */
  defaultShell: '',
};

// ── HTMLTerminalElement ─────────────────────────────────────────────────────

class HTMLTerminalElement extends HTMLElement {
  /**
   * Start a process on a new PTY sized like the terminal. One per element.
   * `env` entries are added to the inherited environment. On POSIX the
   * process is the leader of a new session with the PTY as its controlling
   * terminal; TERM is whatever the environment says (set it in `env`).
   * @param {{command?: string, args?: string[], cwd?: string, env?: Object<string,string>}} options
   * @returns {number} the process id
   * @throws {DOMException} OperationError when it cannot be started or a process already ran here
   */
  spawn(options) {}

  /** Raw bytes (a UTF-8 string) to the process's input, unencoded.
   *  @returns {boolean} false when there is no running process */
  write(data) {}

  /** Bytes into the emulator as if the process had written them (escape
   *  sequences included). Works with or without a process. */
  feed(data) {}

  /** Stop the process (a polite signal, then a forceful one). The screen stays. */
  kill() {}

  /** The live screen as text: rows joined by "\n", trailing blank rows
   *  dropped, trailing spaces trimmed. @returns {string} */
  screenText() {}

  /** History above the screen, oldest first, rows joined by "\n". @returns {string} */
  scrollbackText() {}

  /** The screen as presented (the frame drawn, or about to be): lags
   *  screenText() by a frame, and holds back a synchronized update until it
   *  ends. @returns {string} */
  frameText() {}

  /** Grid size in cells. @type {number} */ cols;
  /** @type {number} */ rows;
  /** The process id, 0 before spawn(). @type {number} */ pid;
  /** Spawned and not yet exited (all its output parsed). @type {boolean} */ running;
  /** The exit status once it has exited (POSIX: 128 + signal when killed),
   *  else null. @type {number|null} */ exitCode;
  /** The title the program set (OSC 0/2). @type {string} */ title;
  /** The cursor in screen cells. @type {{row: number, col: number, visible: boolean, blink: boolean, shape: 'block'|'underline'|'bar'}} */
  cursor;
}

// ── Events (dispatched on the element, not bubbling) ────────────────────────

/** `resize` — the grid changed (the box changed size, or the font).
 *  `event.detail` is `{ cols, rows }`. */
/** `exit` — the process exited and all its output is on the screen.
 *  `event.detail` is `{ exitCode }` (null when the platform gave none). Fires once. */
/** `keydown` / `keyup` / `composition*` / `paste` / `copy` — the usual DOM
 *  events, before the terminal acts; preventDefault() cancels its action. */
