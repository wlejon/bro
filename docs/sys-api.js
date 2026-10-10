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
// 5a. Posting a notification: the web `Notification` API
// ============================================================================
//
// Any app (not only a trusted shell) posts desktop notifications with the
// web API, a global like a browser's. The desktop shows it under the app's
// own identity (bro.json `id` and `name`):
//   - Windows: a toast under the app's AppUserModelID, registered for the
//     current user on first use (HKCU\Software\Classes\AppUserModelId\<id>,
//     DisplayName + IconUri); an app with no id, or a system with toasts off,
//     gets a tray balloon instead.
//   - macOS: UNUserNotificationCenter inside a bundle; outside one, AppleScript's
//     `display notification` (osascript).
//   - Linux: org.freedesktop.Notifications over D-Bus (app name, icon,
//     desktop-entry and suppress-sound hints), else notify-send.
// Headless shows nothing and records each one: `notifications()` in
// docs/headless.md.
//
// A desktop app needs no leave to notify: Notification.permission is
// 'granted' and requestPermission() resolves 'granted'. `show` fires once the
// desktop took it, `error` when nothing could show it, `close` after close().
// A repeated `tag` replaces the earlier notification; requireInteraction keeps
// it up until dismissed. `click` and actions are not delivered yet.
// bro.window.notify(title, body, { icon, silent, timeout, replacesId }) is the
// same path with the native id returned.

const done = new Notification('Download finished', {
    body: 'report.pdf',
    icon: 'assets/done.png',   // app-relative, absolute, or file://; default bro.app.icon
    tag: 'download',
    silent: true,
});
done.onshow = () => console.log('shown');
done.onerror = () => console.log('no notification service');

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
