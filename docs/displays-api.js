/**
 * @file docs/displays-api.js
 * @summary Documentation and examples for the `bro.displays` JavaScript API.
 */

/**
 * `bro.displays` provides display output management, modes, scaling, and night light:
 * - Display snapshot enumeration with modes, geometry, scale, HDR, EDID (`getSnapshot`)
 * - Display configuration changes with test-then-revert semantics (`applyConfig`, `testConfig`, `confirmConfig`, `revertConfig`, `isRevertPending`)
 * - Night light color temperature, schedule, and state (`setNightLight`, `getNightLight`)
 * - Backlight device brightness querying and modification (`setBrightness`, `getBrightness`, `getBacklightDevices`)
 * - Display change event listening (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Gated by the `displays` capability (docs/apps.md, Permissions): an app that declares it in
 * bro.json's `permissions` is asked at first touch; until granted it is the unavailable stub.
 * Compiled in with `BRO_WITH_DISPLAYS`.
 */

// ============================================================================
// 1. Display Snapshot & Inspection
// ============================================================================

if (bro.displays.available) {
    const snapshot = bro.displays.getSnapshot();
    console.log(`Connected displays: ${snapshot.displays.length} (backend: ${snapshot.backend})`);
    for (const d of snapshot.displays) {
        // d.scale is { factor: number, dpi: number }; d.scaleFactor is number
        console.log(`Display: ${d.name} (${d.model || 'Unknown'}), scale: ${d.scaleFactor} (${d.scale.dpi} DPI), primary: ${d.isPrimary}`);
        console.log(`  Current mode: ${d.currentMode.width}x${d.currentMode.height} @ ${d.currentMode.refreshRate}Hz`);
        console.log(`  Orientation: ${d.orientation} (${d.rotation} deg), HDR: ${d.hdr.enabled}`);
    }

    // Subscribe to display layout changes
    bro.displays.on('change', (event) => {
        console.log('Displays changed:', event);
    });
}

// ============================================================================
// 2. Test-Then-Revert Display Configuration
// ============================================================================

// Apply a configuration immediately without a revert timer:
// bro.displays.applyConfig({
//     displayId: 'eDP-1',
//     width: 1920,
//     height: 1080,
//     refreshRate: 60,
//     scaleFactor: 1.25,
//     orientation: 'landscape'
// });

// Test a configuration change with automatic rollback after timeoutMs (default 15000ms):
const testOk = bro.displays.testConfig({
    displayId: 'eDP-1',
    width: 1920,
    height: 1080,
    refreshRate: 60,
    scaleFactor: 1.25
}, { timeoutMs: 10000 });

if (bro.displays.isRevertPending()) {
    // If user confirms within the timeout window:
    // bro.displays.confirmConfig();
    // Or user immediately reverts:
    // bro.displays.revertConfig();
}

// ============================================================================
// 3. Night Light
// ============================================================================

// Configure night light color temperature
bro.displays.setNightLight({
    enabled: true,
    temperature: 4500, // Kelvin (1000..10000)
    schedule: {
        startHour: 20,
        startMinute: 0,
        endHour: 6,
        endMinute: 30
    }
});

const nightLight = bro.displays.getNightLight();
// { supported, enabled, temperature, temperatureKelvin, schedule: { startHour, startMinute, endHour, endMinute } }
console.log('Night light active:', nightLight.enabled, 'temp:', nightLight.temperature);

// ============================================================================
// 4. Backlight & Brightness
// ============================================================================

// Inspect available backlight hardware devices:
const backlights = bro.displays.getBacklightDevices();
// [{ name, sysfsPath, type, currentBrightness, maxBrightness, minBrightness, normalizedBrightness, percent, associatedConnector, connector }]

// Set brightness on primary or named backlight device (0..100% or 0.0..1.0):
bro.displays.setBrightness(80); // 80%

const currentBrightness = bro.displays.getBrightness();
console.log(`Current brightness: ${currentBrightness}%`);
