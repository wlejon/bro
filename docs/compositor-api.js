/**
 * @file docs/compositor-api.js
 * @summary Documentation and examples for the `bro.compositor` JavaScript API.
 */

/**
 * `bro.compositor` provides window-management policies, workspaces, tiling/floating layout modes,
 * and compositor event subscriptions:
 * - Window querying, geometry, and placement (`getWindows`, `getWindow`, `moveWindow`, `resizeWindow`, `closeWindow`)
 * - Focus and window cycling (`focusWindow`, `swapWindows`, `focusDirection`)
 * - Workspace management and switching (`getWorkspaces`, `getWorkspace`, `switchWorkspace`, `createWorkspace`, `moveWindowToWorkspace`)
 * - Layout modes (`setLayoutMode`, `getLayoutMode`, `relayout`)
 * - Monitor arrangement inspection (`getMonitors`)
 * - Compositor event listeners (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_COMPOSITOR` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["compositor", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Windows & Focus
// ============================================================================

if (bro.compositor.available) {
    const windows = bro.compositor.getWindows();
    console.log(`Active windows count: ${windows.length}`);
    for (const win of windows) {
        console.log(`Window #${win.id}: title="${win.title}", app="${win.appId}", workspace=${win.workspaceId}`);
        console.log(`  bounds: ${win.bounds.x},${win.bounds.y} ${win.bounds.width}x${win.bounds.height}, focused=${win.isFocused}`);
    }

    if (windows.length > 0) {
        // Focus a window
        bro.compositor.focusWindow(windows[0].id);

        // Move/resize a floating window
        bro.compositor.moveWindow(windows[0].id, 100, 100);
        bro.compositor.resizeWindow(windows[0].id, 800, 600);
    }
}

// ============================================================================
// 2. Workspaces & Layout Modes
// ============================================================================

if (bro.compositor.available) {
    const workspaces = bro.compositor.getWorkspaces();
    for (const ws of workspaces) {
        console.log(`Workspace ${ws.id}: name="${ws.name}", layout=${ws.layoutMode}, active=${ws.isActive}`);
    }

    // Switch active workspace
    // bro.compositor.switchWorkspace(2);

    // Set tiling layout mode: 'tiling' | 'floating' | 'columns'
    // bro.compositor.setLayoutMode(1, 'tiling');
}

// ============================================================================
// 3. Event Subscriptions
// ============================================================================

if (bro.compositor.available) {
    const sub = bro.compositor.on('windowCreated', (event) => {
        console.log(`New window created: id=${event.windowId}`);
    });

    bro.compositor.on('workspaceChanged', (event) => {
        console.log(`Active workspace changed to ${event.workspaceId}`);
    });

    // Unsubscribe when done
    // sub.remove();
}
