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
 * - Stacking (`getStacking`, `raiseWindow`) and shell-drawn window frames (`setDecorations`,
 *   `getDecorations`, the `data-window-frame` attribute)
 * - Interactive move / resize from script (`beginMove`, `beginResize`, `dragTo`, `endDrag`,
 *   `cancelDrag`, `getDrag`) and snapping (`getSnapping`, `setSnapping`, `snapWindow`,
 *   `snapWindowToward`)
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
 * and its overlays, bottom to top in the window manager's stacking order (`getStacking`), each
 * window's shell-drawn frame (section 7) just below it. One order drives the composite, the hit
 * tests and focus: a new window maps on top and focused, focusing a window (a click, a press on
 * its frame, `focusWindow`) raises it, and closing or minimizing the focused window focuses the
 * most recently used other window. Input routing follows the document, with no ids or classes
 * special:
 * - Pointer: to the shell where the element under the pointer, or an ancestor, has a computed
 *   `z-index` >= 1000 (`pointer-events: none` elements are never hit). Otherwise down the stack
 *   from the top: the first window whose surface is under the pointer gets it (after the
 *   interaction policy below has had its say), unless a window's frame above it is hit first,
 *   which goes to the shell (and raises / focuses that window); below every window, the shell's
 *   desktop. A press's moves and release follow it (an implicit grab), and the cursor is the
 *   client's over a client, the shell's CSS cursor over the shell.
 * - Keyboard: to the focused client window, except while the shell claims it: a rendered element
 *   carries `data-shell-keyboard` (not `="false"`; rendered = no `hidden` attribute or
 *   `display: none` on it or an ancestor, and not `visibility: hidden`), or a rendered text
 *   field / contenteditable in the shell has focus. Global chords are
 *   `bro.window.registerGlobalHotkey` (docs/window-api.js), matched before either.
 * Headless tests can ask the same questions: `shellClaimsPointerAt(x, y)`, `shellClaimsKeyboard()`;
 * with `BRO_HEADLESS_COMPOSITOR=1` a headless shell runs the same compositor (its socket:
 * `hostCompositorSocket()`), and `hostPointer(type, x, y)` drives the same router
 * (docs/headless.md).
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
 * The border and title rules skip windows the shell frames (`framed`, borderless ones too):
 * their frame's own edges and title bar do that. 0 or an empty list switches a rule off.
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
 * for each shell reservation), 'stackingChanged' ({ stacking }: window ids bottom to top),
 * 'snapPreview' ({ windowId, zone, rect }: while a drag has a snap armed, the zone and the rect
 * the window's frame, decoration included, will take; zone 'none' and rect null when it is
 * disarmed or the drag ends), '*' for all.
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

// ============================================================================
// 7. Stacking and shell-drawn window frames
// ============================================================================

/**
 * WindowInfo (getWindow / getWindows / windowCreated / windowChanged) also carries:
 * @typedef {Object} WindowFrameInfo
 * @property {boolean} decorated   the host draws this window's frame: an xdg-decoration
 *                                 server-side window, or an X11 window without its own frame
 * @property {{top:number, left:number, right:number, bottom:number}} decoration
 *                                 the frame band around it right now (zero: no frame)
 * @property {{x:number, y:number, width:number, height:number}} outerFrame
 *                                 frame plus decoration (the rect a frame element takes)
 * @property {boolean} framed      the shell frames it right now (decorated, frames declared,
 *                                 not fullscreen), whether or not its band is zero
 * @property {boolean} borderless  framed with a zero band: the client fills its rect edge to
 *                                 edge and the frame element covers exactly the client
 * @property {string} snap        'none' | 'maximize' | 'left' | 'right' | 'top-left' |
 *                                 'top-right' | 'bottom-left' | 'bottom-right'
 */

/** @returns {number[]} window ids, bottom to top (hidden windows included) */
bro.compositor.getStacking = function () {};

/**
 * Puts a window (and its transients) on top without focusing it.
 * @param {number} id
 * @returns {boolean} false when unknown or already on top
 */
bro.compositor.raiseWindow = function (id) {};

/**
 * The frame the shell draws around decorated windows: how far it reaches past the client on
 * each side (title bar: top; the sides and bottom are typically an invisible resize grab).
 * Maximized windows use `maximizedInsets` (a title bar, or zero). The window manager
 * fits frame and client together: maximize and snapping fill the work area with the whole
 * frame, a new window is fitted inside the work area frame and all (moved in, and shrunk
 * when it opens larger than the area), and the decoration counts in the minimum size.
 * Insets changed while a window is maximized or snapped re-fit it in place. Zero insets in
 * both (the default) mean no frames. Zero in one state only makes that state borderless:
 * the window is still framed (its frame element shows, with `data-window-borderless`,
 * covering exactly the client), so the shell can float controls over it (overlays, below),
 * and a press anywhere on its client is the client's (the interaction policy's title band
 * does not apply to a framed window). A number sets all four sides.
 * @param {{insets?: number|{top?:number,left?:number,right?:number,bottom?:number},
 *          maximizedInsets?: number|{top?:number,left?:number,right?:number,bottom?:number}}} config
 * @returns {{insets:Object, maximizedInsets:Object}} the resulting configuration
 */
bro.compositor.setDecorations = function (config) {};
/** @returns {{insets:Object, maximizedInsets:Object}} */
bro.compositor.getDecorations = function () {};

/**
 * Frame elements. Any element of the shell document carrying `data-window-frame="<window id>"`
 * is that window's frame (the first one naming a window wins). The engine keeps it on the
 * window, in the same frame the window moves, with no script involved:
 * - it is lifted out of the document's stacking order and painted, with its subtree, just
 *   below its window in the window stacking order: the window covers the inside of the frame,
 *   windows above cover the frame, and the frame covers windows below;
 * - it writes, as inline style: `position: fixed`, `box-sizing: border-box`, `left`, `top`,
 *   `width`, `height` (outerFrame), `z-index` (1 + its place in the stack, so DOM hit tests
 *   between frames agree with the composite), and `display: none` while its window shows no
 *   frame (not `framed`: not decorated, no frames declared, fullscreen; or minimized, on a
 *   hidden workspace, unknown id);
 * - and as attributes: `data-window-state` ('normal' | 'maximized'), `data-window-snap`
 *   (the snapped zone; absent when not snapped), `data-window-focused` (present while focused),
 *   `data-window-borderless` (present while its band is zero).
 * Motion: when a framed window changes state (maximized, restored, snapped, unsnapped) its
 * frame glides from the rect it was shown at to the new one (about 220 ms, easing out) rather
 * than jumping, and the window, drawn at its frame's inside, is scaled with it until they
 * meet. The new state's attributes and insets apply at once, so the trim of a window being
 * restored rides the glide. Moves and resizes made by hand are not animated. The window
 * itself moves and takes its new size together, when the client has drawn that size.
 * Overlays: an element inside a frame carrying `data-window-overlay` leaves the frame's paint
 * and is drawn just above the window instead (still under the windows above it), and the
 * pointer over it goes to the shell before the client. That is how a frame floats controls
 * over its client, such as a borderless window's buttons revealed by hovering its corner;
 * keep overlays small and give `pointer-events: none` to any part the client should keep.
 * Everything else (title, buttons, shadow, glow) is the shell's markup and CSS. Contract:
 * - put frames in a container at desktop level (z-index below 1000) with `pointer-events: none`
 *   so the container itself is never hit; give the frames `pointer-events: auto` where they
 *   should take the pointer (a shadow outside the resize band should stay `none`);
 * - do not transform or animate a frame's box (the engine owns its geometry); animate children;
 * - the client is drawn at (left + insets.left, top + insets.top): leave that area to it.
 * A press on a frame reaches the shell's handlers after its window has been raised and focused.
 */

/**
 * Starts an interactive move / resize of a window from the shell (its frame's title bar or
 * edge, in a `mousedown` handler). Until the release, the host routes the pointer to the drag:
 * the window (and its frame) follow, and the release still reaches the shell element that got
 * the press, so its own click / dblclick handlers finish. A move is not `immediate` by default:
 * nothing moves until the pointer travels `dragThreshold` px, so a click or double-click on a
 * title bar stays one. Dragging a maximized or snapped window restores its own size under the
 * pointer (`restoreOnDrag`); a move armed at a monitor edge snaps on release (see setSnapping).
 * A resize of a maximized window does not start.
 * @param {number} id
 * @param {{x?: number, y?: number, immediate?: boolean}} [options]  the pointer the drag starts
 *        from (omitted: where the host's pointer is now)
 * @returns {boolean}
 */
bro.compositor.beginMove = function (id, options) {};
/**
 * @param {number} id
 * @param {string|string[]|number} edges  'top left', ['bottom', 'right'], or resize_edge bits
 *        (1 top, 2 bottom, 4 left, 8 right)
 * @param {{x?: number, y?: number, immediate?: boolean}} [options]  immediate defaults to true
 * @returns {boolean}
 */
bro.compositor.beginResize = function (id, edges, options) {};
/** For hosts that route the pointer themselves (the DRM host does this for you). */
bro.compositor.dragTo = function (x, y) {};
bro.compositor.endDrag = function () {};
/** Ends the drag where the window is, ignoring an armed snap. */
bro.compositor.cancelDrag = function () {};
/**
 * @returns {{windowId:number, action:'move'|'resize', edges:string, active:boolean,
 *            snap:string, snapRect:{x:number,y:number,width:number,height:number}|null}|null}
 */
bro.compositor.getDrag = function () {};

/**
 * Snapping. A moved window's pointer within `edgeThreshold` px of a monitor edge that no other
 * monitor continues past arms a snap: the top edge maximizes, the left / right edges take that
 * half of the work area, and with `cornerSize` > 0 the ends of the side edges take quarters.
 * `snapPreview` reports it while armed, so the shell can draw where the window will land.
 * @typedef {Object} Snapping
 * @property {boolean} enabled        default true
 * @property {number} edgeThreshold   default 8
 * @property {number} cornerSize      default 0 (quarters off)
 * @property {boolean} restoreOnDrag  default true
 */
/** @returns {Snapping} */
bro.compositor.getSnapping = function () {};
/** @param {Partial<Snapping>} options  @returns {Snapping} */
bro.compositor.setSnapping = function (options) {};
/**
 * Fits a window's frame (decoration included) to a zone of its monitor's work area; 'none'
 * returns a snapped or maximized window to the frame it had before. A snapped window follows
 * the work area when reservations or monitors change.
 * @param {number} id
 * @param {'left'|'right'|'maximize'|'top-left'|'top-right'|'bottom-left'|'bottom-right'|'none'} zone
 * @returns {boolean}
 */
bro.compositor.snapWindow = function (id, zone) {};
/**
 * The keyboard step (bind it to Super+arrows): left / right snap to that half (from the other
 * half: restore), up maximizes, down restores a maximized or snapped window and minimizes a
 * normal one.
 * @param {number} id
 * @param {'left'|'right'|'up'|'down'} direction
 * @returns {boolean}
 */
bro.compositor.snapWindowToward = function (id, direction) {};

if (bro.compositor.available) {
    // A 36 px title bar and a 6 px invisible resize band; maximized: the title bar only.
    bro.compositor.setDecorations({ insets: { top: 36, left: 6, right: 6, bottom: 6 },
                                    maximizedInsets: { top: 36 } });
    const layer = document.createElement('div');
    layer.style.cssText = 'position: fixed; inset: 0; z-index: 900; pointer-events: none';
    document.body.appendChild(layer);
    const frames = new Map();
    const addFrame = (w) => {
        if (frames.has(w.id)) return;
        const f = document.createElement('div');
        f.setAttribute('data-window-frame', String(w.id));
        f.style.pointerEvents = 'auto';
        const title = document.createElement('div');
        title.textContent = w.title;
        title.addEventListener('mousedown', () => bro.compositor.beginMove(w.id));
        title.addEventListener('dblclick', () => {
            const now = bro.compositor.getWindow(w.id);
            if (now.maximized) bro.compositor.restoreWindow(w.id);
            else bro.compositor.maximizeWindow(w.id);
        });
        const corner = document.createElement('div');
        corner.addEventListener('mousedown', () => bro.compositor.beginResize(w.id, 'bottom right'));
        f.append(title, corner);
        layer.appendChild(f);
        frames.set(w.id, f);
    };
    bro.compositor.getWindows().forEach(addFrame);
    bro.compositor.on('windowCreated', (e) => addFrame(e.window));
    bro.compositor.on('windowClosed', (e) => {
        frames.get(e.windowId)?.remove();
        frames.delete(e.windowId);
    });
    bro.compositor.on('snapPreview', (e) => {
        // e.rect: where the window will land; null when disarmed.
    });
}
