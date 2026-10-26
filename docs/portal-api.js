/**
 * @file docs/portal-api.js
 * @summary Documentation and examples for the `bro.portal` JavaScript API.
 */

/**
 * `bro.portal` provides an implementation of XDG Desktop Portal backends
 * (`org.freedesktop.impl.portal.*`) for desktop shells:
 * - Backend service lifecycle & interfaces (`start`, `stop`, `isRunning`, `interfaces`, `reason`)
 * - File chooser & OpenURI portal handlers (`onFileChooser`, `offFileChooser`, `onOpenUri`, `offOpenUri`)
 * - Screenshot & Pick Color portal handlers (`onScreenshot`, `offScreenshot`, `onPickColor`, `offPickColor`)
 * - Screencast & PipeWire screen/window sharing portal handler (`onScreencast`, `offScreencast`)
 * - Settings portal (`getSetting`, `setSetting`, `readAllSettings`, `getColorScheme`, `getAccentColor`, `getContrast`, `getReducedMotion`, `onSettingChanged`, `offSettingChanged`)
 * - Inhibit & Session state (`onInhibit`, `offInhibit`, `getActiveInhibitions`, `isInhibited`, `onQueryEndResponse`, `queryEndResponse`, `onStateChanged`, `notifyStateChanged`)
 * - Global Shortcuts & Remote Desktop (`onBindShortcuts`, `onListShortcuts`, `onConfigureShortcuts`, `triggerShortcut`, `releaseShortcut`, `onRemoteDesktop`, `onRemoteDesktopStart`, `onRemoteDesktopInput`)
 *
 * Gated by the `portal` capability, a shell capability (docs/apps.md, Permissions): only the
 * host executable grants it (helm's main, or `--grant portal`); it is never asked for, and for
 * every other app it is the unavailable stub. Compiled in with `BRO_WITH_PORTAL`.
 */

// ============================================================================
// 1. Service Lifecycle & Supported Interfaces
// ============================================================================

if (bro.portal.available) {
    console.log(`Portal service available, running: ${bro.portal.isRunning()}`);
    console.log('Supported D-Bus portal interfaces:', bro.portal.interfaces);

    // Start or stop portal backend server on D-Bus
    // bro.portal.start();
    // bro.portal.stop();
} else {
    console.log('Portal unavailable:', bro.portal.reason);
}

// ============================================================================
// 2. File Chooser & Open URI Handlers
// ============================================================================

if (bro.portal.available) {
    const handle = bro.portal.onFileChooser(async (request) => {
        console.log(`File chooser requested by ${request.appId || 'unknown app'}: title="${request.title}"`);
        console.log(`Multiple: ${request.multiple}, Directory: ${request.directory}`);

        // Return selected URIs or cancelled status
        return {
            uris: ['file:///home/user/Documents/report.pdf'],
            cancelled: false,
        };
    });

    bro.portal.onOpenUri(async (request) => {
        console.log(`Open URI requested: uri=${request.uri}`);
        return { cancelled: false };
    });

    // Unregister handlers with handle.dispose() or bro.portal.offFileChooser() / offOpenUri()
}

// ============================================================================
// 3. Screenshot, Color Picker & Screencast Handlers
// ============================================================================

if (bro.portal.available) {
    bro.portal.onScreenshot(async (request) => {
        console.log(`Screenshot requested, interactive: ${request.interactive}`);
        return {
            uri: 'file:///tmp/screenshot-001.png',
            cancelled: false,
        };
    });

    bro.portal.onPickColor(async (request) => {
        return {
            color: [0.2, 0.4, 0.8], // RGB float array [0..1]
            cancelled: false,
        };
    });

    bro.portal.onScreencast(async (request) => {
        console.log(`Screencast requested, multiple: ${request.multiple}`);
        return {
            streams: [{ node: 42, source_type: 1 }],
            cancelled: false,
        };
    });
}

// ============================================================================
// 4. Settings Portal (`org.freedesktop.impl.portal.Settings`)
// ============================================================================

if (bro.portal.available) {
    // Publish desktop appearance settings to sandboxed/portal clients:
    bro.portal.setSetting('org.freedesktop.appearance', 'color-scheme', 1); // 0=default, 1=dark, 2=light
    bro.portal.setSetting('org.freedesktop.appearance', 'accent-color', [0.21, 0.52, 0.89]);

    const scheme = bro.portal.getColorScheme();     // 'default' | 'prefer-dark' | 'prefer-light'
    const accent = bro.portal.getAccentColor();     // { r, g, b }
    const contrast = bro.portal.getContrast();      // 'normal' | 'high'
    const reduced = bro.portal.getReducedMotion();  // 'no-preference' | 'reduce'
    const allSettings = bro.portal.readAllSettings(['org.freedesktop.appearance']);

    bro.portal.onSettingChanged((ev) => {
        console.log(`Setting changed: ${ev.namespace}.${ev.key} =`, ev.value);
    });
}

// ============================================================================
// 5. Inhibit, Global Shortcuts & Remote Desktop Portals
// ============================================================================

if (bro.portal.available) {
    // Track client inhibit requests (flags: 1=logout, 2=user-switch, 4=suspend, 8=idle):
    bro.portal.onInhibit((req) => {
        console.log(`App ${req.appId} inhibited flags=${req.flags}: ${req.reason}`);
        return true;
    });
    console.log('Active inhibitions:', bro.portal.getActiveInhibitions());
    console.log('Is idle inhibited:', bro.portal.isInhibited('idle'));

    // Broadcast session screensaver / state changes ('running' | 'query-end' | 'ending'):
    // bro.portal.notifyStateChanged(false, 'running');

    // Global shortcuts portal:
    bro.portal.onBindShortcuts(async (req) => {
        // req.shortcuts: [{ id, description, preferredTrigger }]
        return { shortcuts: req.shortcuts, cancelled: false };
    });
    // Trigger or release a bound shortcut id for an app:
    // bro.portal.triggerShortcut('org.example.App', 'toggle-recording');
    // bro.portal.releaseShortcut('org.example.App', 'toggle-recording');

    // Remote desktop portal:
    bro.portal.onRemoteDesktop(async (req) => ({ devices: req.devices || 7, cancelled: false }));
    bro.portal.onRemoteDesktopStart(async (req) => ({ streams: [], devices: 7, cancelled: false }));
    bro.portal.onRemoteDesktopInput((ev) => {
        console.log(`Remote desktop input (${ev.type}) on session ${ev.sessionHandle}`);
    });
}
