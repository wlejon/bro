/**
 * @file docs/portal-api.js
 * @summary Documentation and examples for the `bro.portal` JavaScript API.
 */

/**
 * `bro.portal` provides an implementation of XDG Desktop Portal backends for desktop shells:
 * - File chooser portal handler (`onFileChooser`, `offFileChooser`)
 * - Screenshot & Pick Color portal handler (`onScreenshot`, `offScreenshot`, `onPickColor`, `offPickColor`)
 * - Screencast & PipeWire screen/window sharing portal handler (`onScreencast`, `offScreencast`)
 * - Open URI portal handler (`onOpenUri`, `offOpenUri`)
 * - Backend service lifecycle (`start`, `stop`, `isRunning`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_PORTAL` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["portal", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Service Lifecycle
// ============================================================================

if (bro.portal.available) {
    console.log(`Portal service available, running: ${bro.portal.isRunning()}`);

    // Start or stop portal backend server on D-Bus
    // bro.portal.start();
    // bro.portal.stop();
}

// ============================================================================
// 2. File Chooser Handler
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

    // To unregister the handler later:
    // handle.dispose();
}

// ============================================================================
// 3. Screenshot & Color Picker Handlers
// ============================================================================

if (bro.portal.available) {
    bro.portal.onScreenshot(async (request) => {
        console.log(`Screenshot requested, interactive: ${request.interactive}`);
        // Return screenshot image URI or cancelled status
        return {
            uri: 'file:///tmp/screenshot-001.png',
            cancelled: false,
        };
    });

    bro.portal.onPickColor(async (request) => {
        console.log('Color picker requested');
        return {
            color: [0.2, 0.4, 0.8], // RGB float array
            cancelled: false,
        };
    });
}

// ============================================================================
// 4. Screencast Handler
// ============================================================================

if (bro.portal.available) {
    bro.portal.onScreencast(async (request) => {
        console.log(`Screencast requested, multiple: ${request.multiple}`);
        return {
            streams: [
                { node: 42, source_type: 1 }
            ],
            cancelled: false,
        };
    });
}

// ============================================================================
// 5. Open URI Handler
// ============================================================================

if (bro.portal.available) {
    bro.portal.onOpenUri(async (request) => {
        console.log(`Open URI requested: uri=${request.uri}`);
        return {
            cancelled: false,
        };
    });
}
