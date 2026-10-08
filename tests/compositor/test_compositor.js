// Headless test for bro.compositor
assert(typeof bro.compositor === 'object', 'bro.compositor namespace exists');
if (!bro.compositor.available) {
    skipTest('bro.compositor is unavailable in this environment');
} else {
    // 1. Windows query
    const windows = bro.compositor.getWindows();
    assert(Array.isArray(windows), 'getWindows returns array');

    // 2. Workspaces query
    const workspaces = bro.compositor.getWorkspaces();
    assert(Array.isArray(workspaces), 'getWorkspaces returns array');
    assert(workspaces.length >= 1, 'at least 1 default workspace exists');
    const ws0 = workspaces[0];
    assert(typeof ws0.id === 'number', 'workspace has id');
    assert(typeof ws0.layoutMode === 'string', 'workspace has layoutMode');

    // 3. Layout modes
    assert(typeof bro.compositor.setLayoutMode === 'function', 'setLayoutMode is function');
    assert(typeof bro.compositor.getLayoutMode === 'function', 'getLayoutMode is function');

    // 4. Monitors query
    const monitors = bro.compositor.getMonitors();
    assert(Array.isArray(monitors), 'getMonitors returns array');

    // 5. Events subscription
    let receivedEvent = false;
    const handle = bro.compositor.on('windowCreated', (ev) => {
        receivedEvent = true;
    });
    assert(typeof handle === 'object' && handle !== null, 'on returns handle object');
    assert(typeof handle.remove === 'function', 'handle has remove method');
    handle.remove();

    // 6. Window-management policy: states, edge reservations, interaction.
    for (const name of ['minimizeWindow', 'maximizeWindow', 'fullscreenWindow', 'restoreWindow',
                        'reserveEdge', 'releaseEdge', 'getReservations', 'getWorkArea',
                        'getInteraction', 'setInteraction',
                        'getStacking', 'raiseWindow', 'getDecorations', 'setDecorations',
                        'beginMove', 'beginResize', 'dragTo', 'endDrag', 'cancelDrag', 'getDrag',
                        'getSnapping', 'setSnapping', 'snapWindow', 'snapWindowToward']) {
        assert(typeof bro.compositor[name] === 'function', name + ' is function');
    }
    assert(bro.compositor.maximizeWindow(987654) === false, 'maximizing an unknown window fails');
    const area0 = bro.compositor.getWorkArea();
    assert(area0 && typeof area0.y === 'number', 'getWorkArea returns a rect');
    let reserved = null;
    const rh = bro.compositor.on('reservationChanged', (e) => { reserved = e; });
    const rid = bro.compositor.reserveEdge('top', 34);
    assert(rid > 0, 'reserveEdge returns an id');
    assert(reserved && reserved.id === rid && reserved.shell === true, 'reservationChanged emitted');
    assert(bro.compositor.getWorkArea().y === area0.y + 34, 'work area shrinks by the reservation');
    assert(bro.compositor.releaseEdge(rid) === true, 'releaseEdge');
    assert(bro.compositor.getWorkArea().y === area0.y, 'work area restored');
    rh.remove();
    const ia = bro.compositor.getInteraction();
    assert(ia.titlebarHeight === 38 && ia.resizeBorder === 6, 'interaction defaults');
    assert(bro.compositor.setInteraction({ resizeBorder: 8 }).resizeBorder === 8, 'setInteraction');
    bro.compositor.setInteraction({ resizeBorder: 6 });

    // Stacking, decorations and snapping (the policy half; with no windows).
    assert(Array.isArray(bro.compositor.getStacking()), 'getStacking returns an array');
    assert(bro.compositor.raiseWindow(987654) === false, 'raising an unknown window fails');
    const d = bro.compositor.setDecorations({ insets: { top: 36, left: 6, right: 6, bottom: 6 },
                                              maximizedInsets: { top: 36 } });
    assert(d, 'setDecorations');
    const d2 = bro.compositor.getDecorations();
    assert(d2.insets.top === 36 && d2.insets.left === 6 && d2.maximizedInsets.left === 0, 'decorations round-trip');
    bro.compositor.setDecorations({ insets: 0, maximizedInsets: 0 });
    assert(bro.compositor.getDecorations().insets.top === 0, 'decorations cleared');
    const sn = bro.compositor.getSnapping();
    assert(sn.enabled === true && typeof sn.edgeThreshold === 'number', 'snapping defaults');
    assert(bro.compositor.setSnapping({ edgeThreshold: 12 }).edgeThreshold === 12, 'setSnapping');
    bro.compositor.setSnapping({ edgeThreshold: sn.edgeThreshold });
    assert(bro.compositor.getDrag() === null, 'no drag');
    assert(bro.compositor.beginMove(987654, { x: 0, y: 0 }) === false, 'beginMove of an unknown window fails');
    assert(bro.compositor.snapWindow(987654, 'left') === false, 'snapping an unknown window fails');

    console.log('test_compositor.js PASSED');
}
