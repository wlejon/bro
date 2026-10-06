/**
 * @file docs/sys-api.js
 * @summary Documentation and examples for the `bro.sys` JavaScript API.
 */

/**
 * `bro.sys` provides OS system service integration for desktop shells:
 * - Power / battery state and sleep inhibition (`power`)
 * - Audio device enumeration, volume control, mute, and default sink/source selection (`audio`)
 * - Network state, Wi-Fi scanning, connection, and VPN (`network`)
 * - Bluetooth adapters, devices, pairing, and discovery (`bluetooth`)
 * - Freedesktop Notification Server host (`notifications`)
 * - StatusNotifierItem / System Tray host (`tray`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_SYS` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["sys", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Power & Battery
// ============================================================================

if (bro.sys.available) {
    const power = bro.sys.power.getState();
    console.log('Power state:', {
        onBattery: power.onBattery,
        batteryPercent: power.batteryPercent,
        charging: power.charging,
        powerProfile: power.powerProfile
    });

    // Inhibit system sleep
    const powerInhibitId = bro.sys.power.inhibit('Critical backup running');
    // bro.sys.power.uninhibit(powerInhibitId);
}

// ============================================================================
// 2. Audio Devices & Volume
// ============================================================================

const audioState = bro.sys.audio.getState();
console.log('Audio devices:', audioState.devices);
// Set volume on default sink:
bro.sys.audio.setVolume(audioState.defaultSinkId, 75);

// ============================================================================
// 3. Network & Wi-Fi
// ============================================================================

const netState = bro.sys.network.getState();
console.log('Network connected:', netState.connected, 'type:', netState.primaryType);

// Scan for nearby Wi-Fi access points:
bro.sys.network.scanWifi().then(accessPoints => {
    for (const ap of accessPoints) {
        console.log(`SSID: ${ap.ssid} (signal: ${ap.signalStrength}%, security: ${ap.security})`);
    }
});

// ============================================================================
// 4. Bluetooth
// ============================================================================

const btState = bro.sys.bluetooth.getState();
console.log('Bluetooth powered:', btState.powered);
for (const dev of btState.devices) {
    console.log(`Device: ${dev.name} [${dev.address}] (connected: ${dev.connected})`);
}

// ============================================================================
// 5. Notifications Server Host
// ============================================================================

// Listen for incoming desktop notifications from other apps
bro.sys.notifications.listen((notification) => {
    console.log(`[Notification from ${notification.appName}]: ${notification.summary} - ${notification.body}`);
});

// ============================================================================
// 6. System Tray / StatusNotifierItem Host
// ============================================================================

// Discover and track tray items
const trayItems = bro.sys.tray.getItems();
for (const item of trayItems) {
    console.log(`Tray item: ${item.id} (${item.title || item.tooltip || 'No title'})`);
}
