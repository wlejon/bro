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
// it up until dismissed. bro.window.notify(title, body, { icon, silent,
// timeout, replacesId, actions, payload }) is the same path with the native
// id returned.
//
// Clicks and actions. `actions: [{ action, title }]` adds buttons (at most
// Notification.maxActions, 4; the desktop may show fewer). A click on the
// notification, or on a button, raises and focuses the app's window (restored
// if minimized) and fires `click` with `event.action` ('' for the body, else
// the button's `action`). The user dismissing it fires `close`. A
// notification with no listener for the event — or one an earlier run
// posted — fires `notificationclick` / `notificationclose` on window instead,
// with `event.notification` rebuilt from what was posted (title, body, tag,
// icon, data, actions) and `event.action`. Events are delivered once the page
// has loaded.
//
// If the app has exited, a click starts it: the launch carries the
// notification (`--notification <args>`), and the page gets a
// `notificationclick` on window after load, as for an earlier run. A
// single-instance app that is running when such a launch comes is handed the
// click with it, and hears it as its own (no `instance` event for a launch
// that was only the click). Either way the click reaches the app that posted
// it, never another app or the project manager.
//   - Windows: the toast's launch/action arguments go to a COM activator
//     (INotificationActivationCallback) registered per user under the
//     AppUserModelID (HKCU\Software\Classes\CLSID\{...}\LocalServer32 runs
//     `bro --notification-activated <appDir>`); a running app takes the
//     activation in process. Tray balloons have no click.
//   - Linux: the server's ActionInvoked and NotificationClosed signals (a
//     running app only; the server forgets a notification when its poster
//     exits).
//   - macOS: a UNUserNotificationCenter delegate (inside a bundle). Every bro
//     app is the one bundle, so the bro that hears a click is whichever is
//     running, or a bare bro macOS starts for it when none is; the
//     notification's userInfo names the app that posted it, and a click on
//     another app's starts that app as above (the bare bro then quits). The
//     user dismissing another app's notification is dropped.
// Headless simulates both: clickNotification(id, action?) and
// dismissNotification(id), with the ids from notifications(), and
// notificationActivations() lists what reached the page (docs/headless.md).

const done = new Notification('Download finished', {
    body: 'report.pdf',
    icon: 'assets/done.png',   // app-relative, absolute, or file://; default bro.app.icon
    tag: 'download',
    silent: true,
    data: { path: 'report.pdf' },
    actions: [{ action: 'open', title: 'Open' }, { action: 'folder', title: 'Show in folder' }],
});
done.onshow = () => console.log('shown');
done.onerror = () => console.log('no notification service');
done.onclick = (e) => console.log(e.action === 'folder' ? 'reveal' : 'open', done.data.path);

// Clicks on notifications nothing listens to any more, or from before this run:
window.addEventListener('notificationclick', (e) => {
    console.log('clicked', e.notification.title, e.action, e.notification.data);
});

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
