/**
 * @file docs/compositor-api.js
 * @summary Documentation and examples for the `bro.compositor` JavaScript API.
 */

/**
 * `bro.compositor` is the window-management surface a desktop shell drives.
 * bro provides the mechanism; the shell owns the policy:
 * - Window querying, geometry and placement (`getWindows`, `getWindow`, `moveWindow`, `resizeWindow`, `closeWindow`)
 * - Window states (`minimizeWindow`, `maximizeWindow`, `fullscreenWindow`, `restoreWindow`)
 * - Focus and window cycling (`focusWindow`, `swapWindows`, `focusDirection`)
 * - Workspace management and switching (`getWorkspaces`, `getWorkspace`, `switchWorkspace`, `createWorkspace`,
 *   `removeWorkspace`, `moveWindowToWorkspace`)
 * - Layout modes (`setLayoutMode`, `getLayoutMode`, `relayout`, `setFloating`)
 * - Monitors and the work area (`getMonitors`, `getWorkArea`) and the shell's edge reservations
 *   (`reserveEdge`, `releaseEdge`, `getReservations`)
 * - Pointer-interaction policy for windows the host routes itself (`getInteraction`, `setInteraction`)
 * - Event listeners (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: mounted when `BRO_WITH_COMPOSITOR` is enabled and the app is a trusted shell
 * app declaring `"shell": true` or `"privileged": ["compositor", ...]` in its `bro.json`.
 *
 * Backends: under `bro --drm` (bro.window.displayMode === 'drm') the calls drive bro's own
 * Wayland server (brocompositor over wlroots), which honours everything here. Elsewhere the
 * binding answers from a stand-in window manager with no windows. A call whose command the
 * backend refuses returns false (the Windows / macOS shell backends refuse window states).
 *
 * The DRM shell host: client windows are drawn between the shell document's desktop level
 * and its overlays. Input routing follows the document, with no ids or classes special:
 * - Pointer: to the shell where the element under the pointer, or an ancestor, has a computed
 *   `z-index` >= 1000 (`pointer-events: none` elements are never hit); otherwise to the client
 *   window there, after the interaction policy below has had its say.
 * - Keyboard: to the focused client window, except while the shell claims it: a rendered element
 *   carries `data-shell-keyboard` (not `="false"`; rendered = no `hidden` attribute or
 *   `display: none` on it or an ancestor, and not `visibility: hidden`), or a rendered text
 *   field / contenteditable in the shell has focus. Global chords are
 *   `bro.window.registerGlobalHotkey` (docs/window-api.js), matched before either.
 * Headless tests can ask the same questions: `shellClaimsPointerAt(x, y)`, `shellClaimsKeyboard()`.
 */

// ============================================================================
// 1. Windows & Focus
// ============================================================================

/**
 * @typedef {Object} WindowInfo
 * @property {number} id
 * @property {string} title
 * @property {string} appId
 * @property {{x:number, y:number, width:number, height:number}} frame
 * @property {number} workspaceId
 * @property {number} monitor
 * @property {boolean} focused
 * @property {boolean} minimized
 * @property {boolean} maximized
 * @property {boolean} fullscreen
 * @property {boolean} floating
 * @property {boolean} tiled
 * @property {boolean} shown   false while its workspace is hidden
 */

if (bro.compositor.available) {
    const windows = bro.compositor.getWindows();
    for (const win of windows) {
        console.log(`#${win.id} "${win.title}" (${win.appId}) ws=${win.workspaceId} ` +
                    `${win.frame.width}x${win.frame.height}${win.focused ? ' focused' : ''}`);
    }

    if (windows.length > 0) {
        const id = windows[0].id;
        bro.compositor.focusWindow(id);
        bro.compositor.moveWindow(id, { x: 100, y: 100, width: 800, height: 600 });
        bro.compositor.resizeWindow(id, 800, 600);
    }
}

// ============================================================================
// 2. Window states
// ============================================================================

/**
 * Each returns true when the window exists and the backend accepted the change (also when it
 * is already in that state), false otherwise. The result arrives as `windowChanged`.
 *
 * - maximizeWindow: fills the work area of the window's monitor (bounds minus platform panels
 *   and the shell's `reserveEdge` reservations), remembering the frame it had. A window
 *   maximized this way follows the work area when reservations or monitors change.
 * - fullscreenWindow: covers the whole monitor.
 * - minimizeWindow: hides it; focus moves to the most recent other window on its workspace.
 * - restoreWindow: from minimized, back to the state it was minimized from (switching to its
 *   workspace, focusing it); from maximized / fullscreen, back to the remembered frame (a tiled
 *   window is re-tiled instead).
 *
 * Client requests (a window's own maximize button, a taskbar) go through the same policy.
 * @param {number} id
 * @returns {boolean}
 */
bro.compositor.maximizeWindow = function (id) {};
bro.compositor.fullscreenWindow = function (id) {};
bro.compositor.minimizeWindow = function (id) {};
bro.compositor.restoreWindow = function (id) {};

if (bro.compositor.available) {
    const w = bro.compositor.getWindows()[0];
    if (w) {
        if (w.maximized) bro.compositor.restoreWindow(w.id);
        else bro.compositor.maximizeWindow(w.id);
    }
}

// ============================================================================
// 3. Workspaces & Layout Modes
// ============================================================================

if (bro.compositor.available) {
    for (const ws of bro.compositor.getWorkspaces()) {
        console.log(`Workspace ${ws.id}: "${ws.name}" layout=${ws.layoutMode} active=${ws.active}`);
    }
    // bro.compositor.switchWorkspace(2);
    // Layouts: 'floating' (default: the manager never moves windows) | 'bsp' | 'master-stack' | 'columns' (alias 'tiling') | 'grid'
    // bro.compositor.setLayoutMode(1, 'columns');
}

// ============================================================================
// 4. Monitors, the work area and edge reservations
// ============================================================================

/**
 * Reserves a band along a monitor edge for the shell's own panels (a top bar, a dock that does
 * not auto-hide). The band comes off the work area after the platform's reservations and the
 * shell's earlier ones, so maximize and tiling layouts avoid it; tiled workspaces and
 * maximized windows are re-placed at once.
 * @param {'top'|'bottom'|'left'|'right'} edge
 * @param {number} thickness  pixels (> 0)
 * @param {number} [monitor]  monitor id; omitted: the primary monitor, following it when monitors change
 * @returns {number} reservation id, 0 when refused (bad edge, thickness <= 0)
 */
bro.compositor.reserveEdge = function (edge, thickness, monitor) {};

/**
 * @param {number} id  from reserveEdge
 * @returns {boolean}  false for an unknown id
 */
bro.compositor.releaseEdge = function (id) {};

/**
 * @typedef {Object} Reservation
 * @property {number} id
 * @property {number} monitor     as requested (0: the primary monitor)
 * @property {string} edge
 * @property {number} thickness
 * @property {{x:number, y:number, width:number, height:number}} rect  the granted band (empty while its monitor is absent)
 * @property {boolean} shell      true: a reserveEdge reservation; false: the platform's own (a layer-shell panel)
 * @returns {Reservation[]}
 */
bro.compositor.getReservations = function () {};

/**
 * The work area: monitor bounds minus platform and shell reservations. `getMonitors()` reports
 * the same `workArea` per monitor.
 * @param {number} [monitor]  omitted: the primary monitor
 * @returns {{x:number, y:number, width:number, height:number}|null}
 */
bro.compositor.getWorkArea = function (monitor) {};

if (bro.compositor.available) {
    // The shell's 34 px top bar.
    const bar = bro.compositor.reserveEdge('top', 34);
    bro.compositor.on('reservationChanged', (e) => {
        if (e.id === bar) console.log('bar band is now', e.rect);  // rect is empty once released
    });
    // bro.compositor.releaseEdge(bar);
}

// ============================================================================
// 5. Pointer interaction policy
// ============================================================================

/**
 * How a host that routes the pointer itself (the DRM shell host) reads a press on a client
 * window, before the client gets it:
 * - with any of `dragModifiers` held, a left press anywhere moves the window and a right press
 *   resizes it from its bottom-right corner;
 * - a left press within `resizeBorder` px inside the frame edges resizes from those edges
 *   (not for maximized / fullscreen windows);
 * - a left press within `titlebarHeight` px of the frame top moves the window once the pointer
 *   has travelled a few pixels; the press also reaches the client, so a click there (a CSD
 *   button) is still a click (not for fullscreen windows).
 * 0 or an empty list switches a rule off.
 * @typedef {Object} Interaction
 * @property {number} titlebarHeight   default 38
 * @property {number} resizeBorder     default 6
 * @property {string[]} dragModifiers  default ['alt', 'super']; any of 'ctrl', 'alt', 'shift', 'super'
 */

/** @returns {Interaction} */
bro.compositor.getInteraction = function () {};

/**
 * Changes the given fields; dragModifiers also accepts a string ('super+alt', 'none').
 * @param {Partial<Interaction>} options
 * @returns {Interaction} the resulting policy
 */
bro.compositor.setInteraction = function (options) {};

if (bro.compositor.available) {
    bro.compositor.setInteraction({ titlebarHeight: 32, dragModifiers: ['super'] });
}

// ============================================================================
// 6. Event Subscriptions
// ============================================================================

/**
 * Events: 'windowCreated' (alias 'windowAdded'), 'windowClosed' ('windowRemoved'),
 * 'windowChanged' ({ window, changes }), 'focusChanged', 'workspaceChanged', 'layoutChanged',
 * 'monitorsChanged' ({ monitors }, work areas as getMonitors reports them), 'moveSizeStarted',
 * 'moveSizeEnded', 'reservationChanged' (Reservation fields; also after every monitor change
 * for each shell reservation), '*' for all.
 */
if (bro.compositor.available) {
    const sub = bro.compositor.on('windowCreated', (event) => {
        console.log(`New window created: id=${event.windowId}`);
    });
    bro.compositor.on('workspaceChanged', (event) => {
        console.log(`Active workspace changed to ${event.workspaceId}`);
    });
    // sub.remove();  or  bro.compositor.off(sub);
}
