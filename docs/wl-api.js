/**
 * @file docs/wl-api.js
 * @summary Documentation and examples for the `bro.wl` JavaScript API.
 */

/**
 * `bro.wl` provides Wayland client protocol bindings for shell, panel, lock and dock surfaces:
 * - Foreign toplevel management (`getToplevels`, `on('toplevelAdded')`, activate, close, fullscreen, minimize, maximize)
 * - Layer shell roles (`setLayerRole`, `createLayerSurface` with layers: 'background', 'bottom', 'top', 'overlay')
 * - Screencopy output capture (`captureOutput` returning Promise<{ width, height, stride, format, pixels: Uint8Array }>)
 * - Session lock management (`acquireSessionLock` with `.unlock()` and `.createSurface(outputId?)`)
 * - Idle inhibition and idle notifications (`inhibitIdle`, `createIdleNotification`)
 * - Output, seat, and protocol capability inspection (`getOutputs`, `getSeats`, `hasCompositor`, `hasShm`, `hasLayerShell`, `hasForeignToplevel`, `hasScreencopy`, `hasSessionLock`, `hasIdleInhibit`, `hasIdleNotify`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_WL` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"permissions": ["wl", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Availability, Outputs, Seats & Foreign Toplevels
// ============================================================================

if (bro.wl.available) {
    const outputs = bro.wl.getOutputs(); // [{ id, name, description, make, model, x, y, width, height, scale, refreshMhz }]
    const seats = bro.wl.getSeats();     // [{ id, name, hasPointer, hasKeyboard, hasTouch }]

    const toplevels = bro.wl.getToplevels();
    console.log(`Active toplevels count: ${toplevels.length}`);
    for (const tl of toplevels) {
        // Properties: id, title, appId (app_id), state, parentId, isActivated, isMaximized, isMinimized, isFullscreen, outputs
        console.log(`Toplevel #${tl.id}: title="${tl.title}", app="${tl.appId}", max=${tl.isMaximized}`);
        // Methods: tl.activate(seatId?), tl.close(), tl.setMaximized(), tl.unsetMaximized(),
        //          tl.setMinimized(), tl.unsetMinimized(), tl.setFullscreen(outputId?), tl.unsetFullscreen(), tl.snapshot()
    }

    // Events: 'toplevelAdded' ('toplevelCreated'), 'toplevelChanged' ('toplevelDone'), 'toplevelClosed',
    //         'outputAdded', 'outputRemoved', 'outputChanged', 'seatAdded', 'seatRemoved'
    bro.wl.on('toplevelAdded', (event) => {
        console.log(`Toplevel added: id=${event.toplevel.id}`);
    });
} else {
    console.log('Wayland protocols unavailable:', bro.wl.reason);
}

// ============================================================================
// 2. Layer Shell (Panels, Docks, Lock Screen, Overlays)
// ============================================================================

if (bro.wl.available && bro.wl.hasLayerShell()) {
    // Configure an existing window or layer surface role as a desktop top panel:
    // bro.wl.setLayerRole(windowHandle, {
    //     layer: 'top', // 'background' | 'bottom' | 'top' | 'overlay'
    //     anchor: ['top', 'left', 'right'],
    //     exclusiveZone: 32,
    //     margin: { top: 0, bottom: 0, left: 0, right: 0 },
    //     keyboardInteractivity: 'none' // 'none' | 'exclusive' | 'onDemand'
    // });

    // Or create a standalone layer surface handle:
    // const layer = bro.wl.createLayerSurface({ layer: 'top', namespace: 'panel', width: 1920, height: 32 });
    // layer.setAnchor('top left right');
    // layer.setExclusiveZone(32);
    // layer.commit();
}

// ============================================================================
// 3. Screencopy & Screen Capture
// ============================================================================

if (bro.wl.available && bro.wl.hasScreencopy()) {
    // Capture output by id (optional 2nd arg: { overlayCursor?: boolean } or rect)
    // const frame = await bro.wl.captureOutput(0);
    // console.log(`Captured frame: ${frame.width}x${frame.height}, bytes: ${frame.pixels.length}`);
}

// ============================================================================
// 4. Session Lock & Idle Management
// ============================================================================

if (bro.wl.available && bro.wl.hasSessionLock()) {
    // Acquire ext-session-lock-v1 lock for a lock screen application:
    // const lock = bro.wl.acquireSessionLock();
    // const lockSurface = lock.createSurface(0); // outputId
    // When unlocked after authentication:
    // lock.unlock();
}

if (bro.wl.available && bro.wl.hasIdleInhibit()) {
    // Prevent screen blanking / idle sleep while active:
    // const inhibitor = bro.wl.inhibitIdle();
    // inhibitor.release(); // or inhibitor.restore(), inhibitor.destroy()
}

if (bro.wl.available && bro.wl.hasIdleNotify()) {
    // Create an ext-idle-notify-v1 timer (timeoutMs, seatId?):
    // const idleNote = bro.wl.createIdleNotification(60000);
    // bro.wl.on('idled', (e) => console.log('User idle on notification', e.id));
    // bro.wl.on('resumed', (e) => console.log('User resumed on notification', e.id));
}
