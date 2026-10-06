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
 *
 * Mouse. Unless the program asked for mouse reports, a left drag selects:
 * by character, by word after a double click, by line after a triple click,
 * a rectangular block with Alt held. Shift+click extends the selection, and
 * dragging past the top or bottom edge scrolls. Ctrl+click (Cmd+click on
 * macOS) on a link raises `linkactivate`. When the program did ask (modes
 * 9 / 1000 / 1002 / 1003, encoded as X10, SGR 1006 or SGR-pixels 1016),
 * presses, releases, drags and motion go to it instead. Shift always selects,
 * as in xterm. The wheel scrolls the view through history, `options.wheelLines`
 * lines a notch. A program capturing the mouse gets wheel reports instead,
 * and on the alternate screen under mode 1007 it gets arrow keys. The pointer
 * is an I-beam over text, a hand over links and the default arrow while
 * reporting; a program can choose its own with OSC 22 (`pointerShape`).
 *
 * Rows and ranges. A row number is absolute: it keeps counting as history
 * grows, so it names the same line for as long as that line is in history
 * (`viewport.firstRow` is the oldest still held). A cell is `{row, col}`. A
 * range is `{startRow, startCol, endRow, endCol}`, with the end exclusive:
 * it stops before (endRow, endCol).
 *
 * Scrolling back. While the view is scrolled up into history, new output does
 * not move it. Typing scrolls it back to the bottom unless
 * `options.scrollOnInput` is false.
 *
 * Colours. They come from three places, the later winning slot by slot:
 *   1. bropty's standard palette;
 *   2. CSS: a `background-color` on the element (with its `color` as the
 *      foreground), then the custom properties --terminal-foreground,
 *      --terminal-background, --terminal-cursor, --terminal-selection,
 *      --terminal-match, --terminal-current-match and --terminal-color-0 ...
 *      --terminal-color-255;
 *   3. what script set through `theme`.
 * The result is the base palette, the one a program's own OSC 4 / 10 / 11 /
 * 12 changes reset to (OSC 104 / 110 / 111 / 112). The font is the element's
 * CSS font (family with fallback, size, weight, line-height,
 * letter-spacing). Cells are snapped to whole device pixels at the window's
 * pixel density (devicePixelRatio), so the grid stays crisp on HiDPI.
 *
 * @example
 *   terminal {
 *     font: 14px/1.2 "JetBrains Mono", "Cascadia Mono", monospace;
 *     letter-spacing: 0.5px;
 *     --terminal-background: #1e1e2e; --terminal-foreground: #cdd6f4;
 *     --terminal-color-1: #f38ba8;     --terminal-selection: rgba(137, 180, 250, 0.35);
 *   }
 *
 * Inline images. Programs can show images with the kitty graphics protocol
 * (transmission, placements with z-index, relative placements, deletion,
 * animation frames, Unicode placeholders), sixel, and iTerm2's OSC 1337
 * File=. PNG (kitty f=100) and the iTerm2 formats are decoded by broimage:
 * PNG, JPEG, GIF (every frame), BMP, TGA, PSD, PNM and baseline TIFF (what
 * chafa sends). Each image is drawn on the cells bropty placed it in, so it
 * lands on the text grid at any font size and device scale. Images scroll
 * with their text and leave with it when history evicts it. Kitty
 * placements stay where they were drawn when text is written over them, and
 * their z-index puts them under the cell backgrounds, between the
 * backgrounds and the text, or over the text. Sixel and iTerm2 images are
 * cells: text written over them replaces that part, and they are erased,
 * scrolled and reflowed like text. Animations (kitty frames, animated GIFs)
 * run on the terminal's own thread and present their frames on time
 * without any script. Decoded pixels are held once, shared by every frame
 * that shows them, and uploaded to the GPU once.
 *
 * Memory is bounded per terminal: `options.imageMemoryLimit` (320 MiB by
 * default) caps the decoded RGBA held, counting every frame of every image
 * on both screens. An image that would go over it evicts images without
 * placements first, then the least recently used. One image larger than the
 * whole quota is refused, and the program gets kitty's ENOSPC. Dimensions
 * are capped at 10000 px (4096 for sixel), and an encoded transmission at
 * 256 MiB. Sizes are read from the image headers and checked before
 * anything is decoded. `images` reports what is held. Files, temporary
 * files and shared memory (kitty t=f / t=t / t=s) are refused, because a
 * program on the far side of an ssh connection must not read local files.
 * On Windows the child's output goes through ConPTY, whose console host
 * re-renders it: iTerm2 images (an OSC) pass through, kitty graphics (APC)
 * and sixel (DCS) do not, so those two reach a Windows terminal only from
 * `feed()` (for example, bytes from a socket the page reads itself).
 *
 * @example
 *   // kitty: a 2 x 2 red RGBA image stretched over 6 x 3 cells at the cursor.
 *   const px = btoa(String.fromCharCode(...[255,0,0,255, 255,0,0,255, 255,0,0,255, 255,0,0,255]));
 *   term.feed(`\x1b_Ga=T,f=32,s=2,v=2,c=6,r=3;${px}\x1b\\`);
 *   console.log(term.images);   // { count: 1, placements: 1, bytes: 16, limit: 335544320 }
 *
 * Persistent sessions. spawn({ persistent: true }) runs the program in a
 * session of a bromux server, a per-user process that owns the shells (a
 * Unix socket in the user's runtime directory; on Windows a named pipe only
 * the user can open). The session outlives the element: removing the element,
 * reloading the page or quitting bro detaches it, and attach(id) -- here or
 * in any other page or process, including `bromux attach` -- shows the same
 * screen and scrollback and drives the same program. Several terminals can
 * be attached at once: all see its output, all can type, and its size
 * follows the one most recently active. When the program exits, every
 * terminal attached gets `exit`, and the finished session stays listed (with
 * its exit code) until closeSession(). The server starts on demand from the
 * `bromux` executable built beside bro, and exits by itself 30 s after it
 * has neither a running session nor a client (finished sessions go with it),
 * or at killServer().
 * What crosses bromux: the screen (cells, colours, hyperlinks, cursor,
 * modes, title, cwd), history (fetched as the view needs it), keys, text,
 * paste, mouse, focus, resizes, the bell, notifications (title and body),
 * progress, OSC 133 marks (`promptmark`), OSC 52 (under the element's
 * clipboard policy) and the exit status. The element's theme applies to
 * every colour the program left at its default. What does not cross (yet):
 * inline images, `pointerShape` (OSC 22), `commands` (OSC 133 records),
 * `foregroundProcess`, OSC 99's notification id and urgency; and `feed()`
 * does nothing on a persistent session (its emulator is the server's).
 *
 * @example
 *   // A shell that survives reloads: remember its id, reattach on load.
 *   const term = document.querySelector('terminal');
 *   const saved = Number(localStorage.getItem('shell')) || 0;
 *   if (saved && bro.terminal.sessions().some((s) => s.id === saved && s.running)) {
 *     term.attach(saved);
 *   } else {
 *     term.spawn({ persistent: true, name: 'main shell' });
 *     localStorage.setItem('shell', String(term.sessionId));
 *   }
 *
 * Performance. Every <terminal> is its own compositor layer, recorded only
 * when the terminal changes. A busy terminal therefore re-records only
 * itself, and a page change never re-records a terminal. Set
 * BRO_TERMINAL_LAYER=0 in the environment to paint terminals inline with the
 * page instead; that mode exists only for comparison.
 * `bro.terminal.stats()` and `layerRecords` count the recordings.
 */

// ── bro.terminal ────────────────────────────────────────────────────────────

const terminal = {
  /** Whether <terminal> is compiled into this build. @type {boolean} */
  available: true,
  /** What spawn() runs without a command: %COMSPEC% (else cmd.exe) on Windows,
   *  $SHELL (else /bin/sh) elsewhere. @type {string} */
  defaultShell: '',
  /**
   * Lifetime counters. `layerRecords` counts how often any terminal's layer
   * was recorded. `pageRecords` and `pageInvalidations` count the page's
   * cached paint being re-recorded and being invalidated; those two move
   * only in the windowed frame loop. `layered` is false under
   * BRO_TERMINAL_LAYER=0.
   * @returns {{layered: boolean, layerRecords: number, pageRecords: number, pageInvalidations: number}}
   * @example
   *   const before = bro.terminal.stats();
   *   // ... a busy terminal for a second ...
   *   const after = bro.terminal.stats();
   *   console.log('page re-recorded', after.pageRecords - before.pageRecords, 'times');
   */
  stats() {},

  /** Whether persistent sessions are built in (bro found bromux at build
   *  time). @type {boolean} */
  persistentAvailable: true,
  /**
   * The sessions a bromux server holds (see "Persistent sessions" below):
   * running ones and finished ones not yet closed. [] when the server is not
   * running; this never starts one.
   * @param {{server?: string}|string} [server] the server name; omitted: the per-user default
   * @returns {{id: number, name: string, command: string, pid: number, running: boolean,
   *            exitCode: number|null, cols: number, rows: number, clients: number,
   *            created: number, title: string, cwd: string}[]}
   *   `clients`: how many terminals (here, in other pages or processes, or
   *   `bromux attach`) are attached; `created` is a Unix time in ms.
   * @example
   *   // Reattach to every shell left running by an earlier run of this app.
   *   for (const s of bro.terminal.sessions().filter((s) => s.running && s.name.startsWith('myapp:'))) {
   *     const t = document.createElement('terminal');
   *     document.body.appendChild(t);
   *     t.attach(s.id);
   *   }
   */
  sessions(server) {},
  /** Close a session: its program is killed and the session removed (every
   *  terminal attached to it gets `exit`). @param {number} id
   *  @param {{server?: string}|string} [server] @returns {boolean} */
  closeSession(id, server) {},
  /** Stop the server and every session in it. @param {{server?: string}|string} [server]
   *  @returns {boolean} false when none was running */
  killServer(server) {},
};

// ── HTMLTerminalElement ─────────────────────────────────────────────────────

class HTMLTerminalElement extends HTMLElement {
  /**
   * Start a process on a new PTY sized like the terminal. One per element
   * (after a detach() the element takes another). `env` entries are added
   * to the inherited environment. On POSIX the process is the leader of a
   * new session with the PTY as its controlling terminal; TERM is whatever
   * the environment says (set it in `env`).
   *
   * With `persistent: true` the process runs in a session of a bromux
   * server instead (see "Persistent sessions"), which outlives this element:
   * `server` names the server (omitted: the per-user default, started on
   * demand from the `bromux` executable beside bro), `name` is the
   * session's display name in sessions(). `sessionId` then names it.
   * @param {{command?: string, args?: string[], cwd?: string, env?: Object<string,string>,
   *          persistent?: boolean, server?: string, name?: string}} options
   * @returns {number} the process id
   * @throws {DOMException} OperationError when it cannot be started or a process already ran here
   */
  spawn(options) {}

  /**
   * Show and drive a persistent session: one from spawn({persistent}) in
   * this page, an earlier page, another process, or `bromux new`. The
   * screen and scrollback are the session's; input, resizes, the mouse,
   * focus reports and the clipboard go to it, and its events come here.
   * Replaces whatever finished or detached session this element had.
   * @param {number} sessionId
   * @param {{server?: string}|string} [server]
   * @throws {DOMException} OperationError when there is no such session or
   *   server, or this element's process is running
   */
  attach(sessionId, server) {}

  /** Let go of the persistent session; its program runs on, and `detach`
   *  fires. The screen stays as it was. Removing the element from the
   *  document, reloading the page or closing bro does the same. A local
   *  (non-persistent) process is not affected. */
  detach() {}

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
  /** The persistent session this element shows (attached or detached
   *  from), null for a local process. @type {number|null} */ sessionId;
  /** Spawned and not yet exited (all its output parsed). @type {boolean} */ running;
  /** The exit status once it has exited (POSIX: 128 + signal when killed),
   *  else null. @type {number|null} */ exitCode;
  /** The title the program set (OSC 0/2). @type {string} */ title;
  /** The cursor in screen cells. @type {{row: number, col: number, visible: boolean, blink: boolean, shape: 'block'|'underline'|'bar'}} */
  cursor;
  /** The working directory the shell reported (OSC 7), as a path; "" when
   *  none. @type {string} */
  cwd;
  /** The process that owns the terminal now: the shell at its prompt, the
   *  program it is running while it runs. null before spawn() and after exit.
   *  `name` is what the process goes by (argv[0]'s base name on POSIX, so a
   *  node program that set process.title is known by that title; the
   *  executable's file name on Windows), `path` its executable ("" when not
   *  readable), `commandLine` how it was started (POSIX: argv, shell-quoted).
   *  POSIX: the leader of the terminal's foreground process group -- the
   *  job the shell started, whose own children share its group (`make`
   *  while it runs `cc`). Windows has no foreground group: the answer is the
   *  youngest console program in the child's process tree (`cc` under
   *  `make`), skipping GUI programs the shell started; a console program
   *  started in the background (`start /b`) counts while it is the youngest.
   *  Checked shortly after input or output and every few seconds otherwise,
   *  so it trails a change by up to ~2 s when nothing is printed, and fires
   *  `foregroundchange` when it changes.
   *  @type {{pid: number, name: string, path: string, commandLine: string}|null}
   *  @example
   *    term.addEventListener('foregroundchange', (e) => {
   *      tab.label = e.detail.process ? e.detail.process.name : 'exited';
   *    }); */
  foregroundProcess;
  /** The pointer the program asked for with OSC 22 (a CSS cursor name), ""
   *  when it did not. @type {string} */
  pointerShape;
  /** The cell box in CSS px (fractional, snapped to device pixels), the
   *  baseline from the cell top, the cell in device pixels (what the PTY's
   *  window size and SGR-pixel reports count), and the render scale.
   *  @type {{cellWidth: number, cellHeight: number, baseline: number, pixelWidth: number, pixelHeight: number, scale: number}} */
  metrics;
  /** How often this terminal's layer was recorded (see Performance). @type {number} */
  layerRecords;

  /** Text into the process as a paste: bracketed (ESC[200~ ... ESC[201~)
   *  when the program set mode 2004, with control characters (ESC among them)
   *  removed, so the pasted text cannot end the bracket itself.
   *  Scrolls to the bottom like typing. @returns {boolean} false with no process */
  paste(text) {}

  // ── Options ──

  /**
   * Behaviour switches. Reading gives a fresh object; assigning merges the
   * keys given, and the rest keep their values. A value out of range throws
   * TypeError.
   *  - copyOnSelect (false): a finished selection is copied to the clipboard
   *    (and, on Linux, to the primary selection).
   *  - middleClickPaste (true on Linux, else false): a middle click pastes
   *    (from the primary selection on Linux).
   *  - scrollOnInput (true): typing returns a scrolled-back view to the bottom.
   *  - boldIsBright (false): bold text in one of the eight base colours draws
   *    in its bright twin (xterm's boldColors).
   *  - minimumContrast (1, range 1-21): the WCAG contrast ratio every glyph
   *    keeps against its background. A foreground below it has its lightness
   *    moved, hue kept (brothemes, Oklch). 1 turns it off; 4.5 is WCAG AA.
   *  - ligatures (false): programming ligatures across cells (calt/liga),
   *    with the font's own advance as the cell width.
   *  - clipboard ('write'): which OSC 52 requests reach the page. 'deny'
   *    allows none, 'write' lets a program set the clipboard, and
   *    'read-write' also lets it ask for the clipboard (see clipboardread).
   *  - wheelLines (3, range 1-100): lines one wheel notch scrolls.
   * @type {{copyOnSelect: boolean, middleClickPaste: boolean, scrollOnInput: boolean, boldIsBright: boolean,
   *         minimumContrast: number, ligatures: boolean, clipboard: 'deny'|'write'|'read-write', wheelLines: number}}
   * @example
   *   term.options = { copyOnSelect: true, minimumContrast: 4.5, clipboard: 'read-write' };
   */
  options;

  /**
   * Colours. Reading gives the resolved colours in effect: "#rrggbb"
   * strings, overlays as "rgba(...)", and `ansi` with all 256 entries
   * (0-15 the ANSI colours, 16-255 the xterm table). Assigning sets script's
   * own values. Each key given replaces its slot, a null or "" entry hands
   * the slot back to CSS, and keys left out keep what script set before.
   * Assigning null clears everything script set. Any CSS colour is accepted.
   * `ansi` may be sparse and has at most 256 entries.
   * @type {{foreground: string, background: string, cursor: string, selection: string, match: string,
   *         currentMatch: string, ansi: string[]}}
   * @example
   *   term.theme = {
   *     background: '#002b36', foreground: '#839496', cursor: '#93a1a1',
   *     ansi: ['#073642', '#dc322f', '#859900', '#b58900', '#268bd2', '#d33682', '#2aa198', '#eee8d5'],
   *   };
   *   const table = []; table[208] = 'orange'; term.theme = { ansi: table };  // one slot of the 256
   *   term.theme = null;                                                      // back to CSS
   */
  theme;

  // ── Scrollback view ──

  /** Where the view is. `topRow` is the first row shown and `firstRow` the
   *  oldest row in history. `screenTopRow` is the live screen's first row,
   *  `rows` the screen height and `historyRows` the rows held above the
   *  screen. Use these for a scrollbar: the thumb spans `rows` of
   *  `historyRows + rows`, starting at `topRow - firstRow`.
   *  @type {{topRow: number, firstRow: number, screenTopRow: number, rows: number, historyRows: number, atBottom: boolean, altScreen: boolean}} */
  viewport;
  /** Scroll by `n` lines (negative: up into history). @returns {boolean} whether the view moved */
  scrollLines(n) {}
  /** Scroll by `n` screens. @returns {boolean} whether the view moved */
  scrollPages(n) {}
  /** The oldest row held. @returns {boolean} whether the view moved */
  scrollToTop() {}
  /** Back to the live screen. @returns {boolean} whether the view moved */
  scrollToBottom() {}
  /** Bring `row` (absolute) to the top of the view, clamped. @returns {boolean} whether the view moved */
  scrollToRow(row) {}
  /** To the previous (`direction` < 0) or next shell prompt (OSC 133).
   *  @returns {boolean} false when there is none that way */
  scrollToPrompt(direction) {}

  // ── Selection ──

  /** Select a range. */
  select(range) {}
  /** Select history and screen. */
  selectAll() {}
  clearSelection() {}
  /** Select the output of the command at cell (row, col), or of the last
   *  command when called with no arguments (OSC 133 marks needed).
   *  @returns {boolean} false when there is no such command */
  selectOutput(row, col) {}
  /** The selection, null when none. @type {{startRow: number, startCol: number, endRow: number, endCol: number}|null} */
  selection;
  /** The selected text, lines joined by "\n" (wrapped lines rejoined). @returns {string} */
  selectionText() {}
  /** The text of any range. @returns {string} */
  textInRange(range) {}

  // ── Search ──

  /**
   * Search the whole buffer: the screen at once, history in the background
   * on the parser thread. `searchchange` reports progress, and matches in new
   * output are added as it arrives. All matches are highlighted and the
   * current one more strongly (theme.match / theme.currentMatch).
   * An empty pattern clears the search. `caseMode` is 'smart' (the default:
   * case-insensitive unless the pattern has an uppercase letter), 'sensitive'
   * or 'insensitive'. `wholeWord` requires a match not to touch word
   * characters on either side. A regex is brosearch syntax (RE2-like,
   * Unicode).
   * @param {string} pattern
   * @param {{regex?: boolean, caseMode?: 'smart'|'sensitive'|'insensitive', wholeWord?: boolean}} [options]
   * @returns {boolean}
   * @throws {DOMException} SyntaxError for a bad regex; TypeError for an unknown caseMode
   * @example
   *   term.search('error\\s+\\d+', { regex: true, caseMode: 'insensitive' });
   *   term.addEventListener('searchchange', (e) => counter.textContent =
   *     e.detail.count ? `${(e.detail.current ?? -1) + 1} / ${e.detail.count}` : 'no matches');
   *   nextButton.onclick = () => term.searchNext();
   */
  search(pattern, options) {}
  clearSearch() {}
  /** The next match after the current one (wrapping), made current and
   *  scrolled into view; null when there is none. @returns {{startRow: number, startCol: number, endRow: number, endCol: number}|null} */
  searchNext() {}
  /** The previous match. @returns {{startRow: number, startCol: number, endRow: number, endCol: number}|null} */
  searchPrevious() {}
  /** `current` is the index of the current match in buffer order (null
   *  before searchNext), and `range` is that match.
   *  @type {{active: boolean, complete: boolean, count: number, current: number|null, range: object|null, pattern: string}} */
  searchStatus;

  // ── Links ──

  /** The link at a cell: an OSC 8 hyperlink ('hyperlink'), or a URL ('url')
   *  or file path ('path') found in the text. `uri` is the target and `text`
   *  what is shown. Null when there is none. Hovering a link underlines it.
   *  @returns {{range: object, kind: 'hyperlink'|'url'|'path', uri: string, text: string}|null} */
  linkAt(row, col) {}

  // ── Shell integration (OSC 133) ──

  /** The shell's commands, oldest first. Each one has the position of its
   *  prompt (A), its input (B), its output (C) and its end (D), its exit
   *  code (null until it finishes, or when the shell gave none), and its
   *  command line. The command line is the one the shell reported
   *  (`cmdline=` / OSC 633;E), else the text typed between B and C. The
   *  last entry is usually the prompt now waiting.
   *  @type {{prompt: {row: number, col: number}, input: object|null, output: object|null, end: object|null,
   *          exitCode: number|null, finished: boolean, commandLine: string}[]}
   *  @example
   *    // bash: mark prompts the way shell-integration scripts do.
   *    term.spawn({ command: 'bash', env: {
   *      PS1: '\\[\\e]133;A\\a\\]\\w$ \\[\\e]133;B\\a\\]', PS0: '\\[\\e]133;C\\a\\]',
   *      PROMPT_COMMAND: 'printf "\\033]133;D;%s\\007" $?' } });
   *    // later: what failed?
   *    term.commands.filter((c) => c.exitCode).forEach((c) => console.log(c.commandLine, c.exitCode));
   */
  commands;

  // ── Clipboard reads (OSC 52) ──

  /** Answer a `clipboardread` the page cancelled. @returns {boolean} false when the id is unknown */
  answerClipboard(id, text) {}
  /** Refuse it: the program gets no reply, as when the policy refuses. @returns {boolean} */
  denyClipboard(id) {}
}

// ── Events (dispatched on the element, not bubbling) ────────────────────────

/** `resize`: the grid changed (the box changed size, or the font).
 *  `event.detail` is `{ cols, rows }`. */
/** `exit`: the process exited and all its output is on the screen.
 *  `event.detail` is `{ exitCode }` (null when the platform gave none). Fires once. */
/** `keydown` / `keyup` / `composition*` / `paste` / `copy`: the usual DOM
 *  events, before the terminal acts; preventDefault() cancels its action. */
/** `titlechange`: the program set its title (OSC 0 / 2). detail `{ title }`. */
/** `cwdchange`: the shell reported its directory (OSC 7). detail `{ cwd }`. */
/** `detach`: the element let go of its persistent session (detach(), or it
 *  left the document) while the session's program still runs. detail
 *  `{ sessionId }`. */
/** `foregroundchange`: `foregroundProcess` changed. detail `{ process }`,
 *  the new value (null once the child has exited). */
/** `bell`: BEL. detail `{}`. */
/** `notification`: a desktop notification. OSC 9 carries a body; OSC 777
 *  (notify;title;body) and OSC 99 (kitty) carry title and body. detail
 *  `{ title, body, id, source, urgency }`. bro shows nothing; the page decides. */
/** `progress`: OSC 9;4 progress (ConEmu / Windows Terminal). detail
 *  `{ state: 'none'|'normal'|'error'|'indeterminate'|'paused', value }`,
 *  with value 0-100. */
/** `clipboardwrite`: the program set the clipboard (OSC 52), under
 *  options.clipboard 'write' or 'read-write'. detail `{ text, selection }`,
 *  where selection is OSC 52's field: 'c' clipboard, 'p' primary, and so on.
 *  Cancelable. If not cancelled, bro writes it: to the clipboard, and to the
 *  primary selection when only 'p' is named.
 *  @example
 *    term.addEventListener('clipboardwrite', (e) => {
 *      if (!confirmClipboard(e.detail.text)) e.preventDefault();
 *    }); */
/** `clipboardread`: the program asked for the clipboard (OSC 52 `?`), under
 *  options.clipboard 'read-write' only. detail `{ id, selection }`.
 *  Cancelable. If not cancelled, bro answers with the clipboard's text.
 *  Cancelled, the request waits for answerClipboard(id, text) or
 *  denyClipboard(id).
 *  @example
 *    term.options = { clipboard: 'read-write' };
 *    term.addEventListener('clipboardread', (e) => {
 *      e.preventDefault();
 *      askUser().then((ok) => ok ? term.answerClipboard(e.detail.id, myText) : term.denyClipboard(e.detail.id));
 *    }); */
/** `promptmark`: an OSC 133 mark. detail `{ mark: 'A'|'B'|'C'|'D', params,
 *  exitCode }`; exitCode is set on D. `commands` holds the marks assembled. */
/** `linkactivate`: Ctrl+click (Cmd+click on macOS) on a link. detail
 *  `{ uri, kind, text }`. Cancelable. If not cancelled, bro opens it
 *  through the OS: URLs in the browser, paths with their default app
 *  (relative paths resolved against `cwd`). Headless never opens anything.
 *  @example
 *    term.addEventListener('linkactivate', (e) => {
 *      if (e.detail.kind === 'path') { e.preventDefault(); openInEditor(e.detail.uri); }
 *    }); */
/** `scroll`: the view moved, history grew, or the screen switched. detail
 *  is `viewport`'s object. */
/** `searchchange`: the search's status changed (progress, count, current).
 *  detail `{ active, complete, count, current }`. */
