/**
 * @file docs/wl-api.js
 * @summary Documentation and examples for the `bro.wl` JavaScript API.
 */

/**
 * `bro.wl` provides Wayland client protocol bindings for shell, panel, lock and dock surfaces:
 * - Foreign toplevel management (`getToplevels`, `on('toplevelAdded')`, activate, close, fullscreen, minimize, maximize)
 * - Layer shell roles (`setLayerRole`, `createLayerSurface` with layers: 'background', 'bottom', 'top', 'overlay')
 * - Screencopy output capture (`captureOutput` returning Promise<{ width, height, stride, format, pixels: Uint8Array }>)
 * - Session lock management (`acquireSessionLock` with `.unlock()`)
 * - Idle inhibition and idle notifications (`inhibitIdle`, `createIdleNotification`)
 * - Output and seat enumeration (`getOutputs`, `getSeats`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_WL` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["wl", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Foreign Toplevels
// ============================================================================

if (bro.wl.available) {
    const toplevels = bro.wl.getToplevels();
    console.log(`Active toplevels count: ${toplevels.length}`);
    for (const tl of toplevels) {
        console.log(`Toplevel #${tl.id}: title="${tl.title}", app="${tl.appId}", max=${tl.maximized}`);
    }

    // Subscribe to toplevel additions
    bro.wl.on('toplevelAdded', (event) => {
        console.log(`Toplevel added: id=${event.toplevel.id}`);
    });
}

// ============================================================================
// 2. Layer Shell (Panels, Docks, Lock Screen, Overlays)
// ============================================================================

if (bro.wl.available) {
    // Configure window role as a desktop top panel
    // bro.wl.setLayerRole(windowHandle, {
    //     layer: 'top',
    //     anchor: ['top', 'left', 'right'],
    //     exclusive: true,
    //     margin: { top: 0, bottom: 0, left: 0, right: 0 }
    // });
}

// ============================================================================
// 3. Screencopy & Screen Capture
// ============================================================================

if (bro.wl.available && bro.wl.hasScreencopy && bro.wl.hasScreencopy()) {
    // Capture output by id
    // const frame = await bro.wl.captureOutput(0);
    // console.log(`Captured frame: ${frame.width}x${frame.height}, bytes: ${frame.pixels.length}`);
}

// ============================================================================
// 4. Session Lock
// ============================================================================

if (bro.wl.available && bro.wl.hasSessionLock && bro.wl.hasSessionLock()) {
    // Acquire session lock for lock screen application
    // const lock = bro.wl.acquireSessionLock();
    // When unlocked:
    // lock.unlock();
}
