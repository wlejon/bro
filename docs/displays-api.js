/**
 * @file docs/displays-api.js
 * @summary Documentation and examples for the `bro.displays` JavaScript API.
 */

/**
 * `bro.displays` provides display output management, modes, scaling, and night light:
 * - Display snapshot enumeration with modes, geometry, scale, EDID (`getSnapshot`)
 * - Display configuration changes with test-then-revert semantics (`applyConfig`, `testConfig`, `confirmConfig`, `revertConfig`)
 * - Night light color temperature, schedule, and state (`setNightLight`, `getNightLight`)
 * - Backlight device brightness querying and modification (`setBrightness`, `getBrightness`, `getBacklightDevices`)
 * - Display change event listening (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_DISPLAYS` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["displays", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Display Snapshot & Inspection
// ============================================================================

if (bro.displays.available) {
    const snapshot = bro.displays.getSnapshot();
    console.log(`Connected displays: ${snapshot.displays.length}`);
    for (const d of snapshot.displays) {
        console.log(`Display: ${d.name} (${d.model || 'Unknown'}), scale: ${d.scale}, primary: ${d.isPrimary}`);
        console.log(`  Current mode: ${d.currentMode.width}x${d.currentMode.height} @ ${d.currentMode.refreshRate}Hz`);
    }

    // Subscribe to display layout changes
    bro.displays.on('change', (event) => {
        console.log('Displays changed:', event);
    });
}

// ============================================================================
// 2. Test-Then-Revert Display Configuration
// ============================================================================

// Test a resolution change with automatic rollback after 10 seconds:
const testPromise = bro.displays.testConfig({
    displayId: 'eDP-1',
    mode: { width: 1920, height: 1080, refreshRate: 60 },
    scale: 1.25
}, { revertAfterMs: 10000 });

// If user confirms:
// bro.displays.confirmConfig();
// Or user immediately reverts:
// bro.displays.revertConfig();

// ============================================================================
// 3. Night Light
// ============================================================================

// Configure night light color temperature
bro.displays.setNightLight({
    enabled: true,
    temperature: 4500, // Kelvin
    schedule: {
        startHour: 20,
        startMinute: 0,
        endHour: 6,
        endMinute: 30
    }
});

const nightLight = bro.displays.getNightLight();
console.log('Night light active:', nightLight.enabled, 'temp:', nightLight.temperature);

// ============================================================================
// 4. Backlight & Brightness
// ============================================================================

// Set brightness on primary or named display
bro.displays.setBrightness(80); // 80%

const currentBrightness = bro.displays.getBrightness();
console.log(`Current brightness: ${currentBrightness}%`);
