/**
 * @file docs/sys-api.js
 * @summary Documentation and examples for the `bro.sys` JavaScript API.
 */

/**
 * `bro.sys` provides desktop system daemon and hardware state integration across
 * 7 sub-namespaces:
 * - `bro.sys.power`: Battery/AC state, power actions (suspend/reboot/powerOff), and inhibitors
 * - `bro.sys.audio`: PipeWire / PulseAudio device enumeration, volume, mute, and default routing
 * - `bro.sys.network`: NetworkManager / OS interfaces, Wi-Fi scanning/connecting, and VPN control
 * - `bro.sys.bluetooth`: BlueZ adapter control, discovery, pairing, and device connection
 * - `bro.sys.notifications`: Desktop notification daemon (`org.freedesktop.Notifications`), history, and DND
 * - `bro.sys.tray`: StatusNotifierWatcher / system tray host, item activation, and DBusMenu inspection
 * - `bro.sys.processes`: Process table inspection and signaling (`list`, `kill`, plus top-level aliases)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_SYS` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"permissions": ["sys", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Power Management (`bro.sys.power`)
// ============================================================================

const powerState = bro.sys.power.getState();
// {
//   source: 'ac' | 'battery' | 'ups' | 'unknown',
//   percent: number,
//   timeToEmptyS: number,
//   timeToFullS: number,
//   lidPresent: boolean,
//   lidClosed: boolean,
//   hasSystemBattery: boolean,
//   devices: [{
//     id, kind, powerSupply, state, technology, percent,
//     timeToEmptyS, timeToFullS, energyWh, energyFullWh,
//     energyFullDesignWh, energyRateW, vendor, model, serial
//   }]
// }
console.log(`Power source: ${powerState.source} (${powerState.percent}%)`);

const powerCaps = bro.sys.power.getCapabilities();
// { suspend, hibernate, hybridSleep, reboot, powerOff, lock }

// Request a power action ('suspend' | 'hibernate' | 'hybridSleep' | 'reboot' | 'powerOff' | 'lock'):
// bro.sys.power.request('suspend');

// Inhibit sleep/idle (returns numeric inhibitor ID):
const inhibitId = bro.sys.power.inhibit('Playing video', 'MyPlayer', 'sleep:idle', 'block');
console.log('Inhibited:', bro.sys.power.isInhibited());
bro.sys.power.uninhibit(inhibitId);

// Events: 'changed' (alias 'powerChanged'), 'capabilitiesChanged', 'sleepPrepare', 'shutdownPrepare'
bro.sys.power.on('changed', (state) => {
    console.log('Power state updated:', state.percent);
});

// ============================================================================
// 2. Audio Routing & Volume (`bro.sys.audio`)
// ============================================================================

const audioState = bro.sys.audio.getState();
// { defaultOutput: string, defaultInput: string, devices: AudioDevice[] }

const audioDevices = bro.sys.audio.getDevices();
for (const dev of audioDevices) {
    // {
    //   id, description, deviceName, formFactor,
    //   direction: 'output' | 'input', state, isDefault,
    //   hasVolume, volume, muted, nativeId, channelVolumes
    // }
    console.log(`[${dev.direction}] ${dev.description}: ${Math.round(dev.volume * 100)}% (muted: ${dev.muted})`);
}

// Set volume (accepts 0.0..1.0 or 0..100), mute, and default devices:
if (audioState.defaultOutput) {
    bro.sys.audio.setVolume(audioState.defaultOutput, 0.75);
    bro.sys.audio.setMute(audioState.defaultOutput, false);
    bro.sys.audio.setDefaultSink(audioState.defaultOutput); // or setDefault(id), setDefaultSource(id)
}

// Events: 'deviceAdded', 'deviceRemoved', 'deviceChanged', 'defaultChanged'
bro.sys.audio.on('deviceChanged', (dev) => {
    console.log('Audio device changed:', dev.id, dev.volume);
});

// ============================================================================
// 3. Network & Wi-Fi (`bro.sys.network`)
// ============================================================================

const netState = bro.sys.network.getState();
// {
//   connectivity: 'none' | 'portal' | 'limited' | 'full' | 'unknown',
//   networkingEnabled: boolean,
//   wifiEnabled: boolean,
//   wifiHardwareEnabled: boolean,
//   primaryDevice: string,
//   devices: [{
//     id, interfaceName, description, type, state, mac,
//     connection, speedMbps, ipv4: { addresses, gateways, dns },
//     ipv6: { addresses, gateways, dns }, isPrimary, managed
//   }],
//   activeConnections: [{ id, name, uuid, type, state, deviceIds, default4, default6 }]
// }

// List or scan Wi-Fi access points:
const accessPoints = bro.sys.network.getAccessPoints();
// [{ deviceId, ssid, bssid, strengthPercent, rssiDbm, frequencyMhz, channel, security, active }]

const scannedAps = await bro.sys.network.scanWifi();
// Connect to a Wi-Fi network (returns Promise<boolean>):
// await bro.sys.network.connectWifi('MySSID', 'MyPassphrase');

// Disconnect interface / Wi-Fi / VPN:
// bro.sys.network.disconnect(netState.primaryDevice);
// bro.sys.network.disconnectWifi();
// bro.sys.network.connectVpn('WorkVPN');
// bro.sys.network.disconnectVpn('WorkVPN');

// Events: 'changed' (alias 'networkChanged'), 'wifiScanCompleted'
bro.sys.network.on('changed', (state) => {
    console.log('Network connectivity:', state.connectivity);
});

// ============================================================================
// 4. Bluetooth (`bro.sys.bluetooth`)
// ============================================================================

const btState = bro.sys.bluetooth.getState();
// { adapters: BluetoothAdapter[], devices: BluetoothDevice[], defaultAdapter: string }

const adapters = bro.sys.bluetooth.getAdapters();
// [{ id, address, name, alias, powered, discovering, pairable, discoverable }]

const btDevices = bro.sys.bluetooth.getDevices();
// [{ id, adapterId, mac, name, alias, icon, paired, connected, trusted, blocked, rssi }]

// Control adapter power & discovery:
// bro.sys.bluetooth.setPowered(true);
// bro.sys.bluetooth.startDiscovery();
// bro.sys.bluetooth.stopDiscovery();

// Pair / connect / disconnect / remove by MAC address (each returns Promise<boolean>):
// await bro.sys.bluetooth.pair('AA:BB:CC:DD:EE:FF');
// await bro.sys.bluetooth.connect('AA:BB:CC:DD:EE:FF');
// await bro.sys.bluetooth.disconnect('AA:BB:CC:DD:EE:FF');
// await bro.sys.bluetooth.removeDevice('AA:BB:CC:DD:EE:FF');

// Events: 'adapterChanged', 'deviceFound', 'deviceChanged', 'deviceRemoved'
bro.sys.bluetooth.on('deviceChanged', (dev) => {
    console.log(`Bluetooth device ${dev.name}: connected=${dev.connected}`);
});

// ============================================================================
// 5. Desktop Notifications Daemon (`bro.sys.notifications`)
// ============================================================================

// Start listening as the session notification server:
bro.sys.notifications.listen({ replaceExisting: true, doNotDisturb: false });

const notifCaps = bro.sys.notifications.getCapabilities();
// { receivesForeign: boolean, source: string, detail: string }

// Post a local notification (returns numeric notification ID):
const notifId = bro.sys.notifications.post({
    appName: 'System Shell',
    summary: 'Battery Low',
    body: '15% remaining',
    urgency: 'normal', // 'low' | 'normal' | 'critical'
    timeoutMs: 5000,
    actions: [{ key: 'open-power', label: 'Power Settings' }]
});

// Inspect active and historical notifications:
const activeNotifs = bro.sys.notifications.getActive();
const historyNotifs = bro.sys.notifications.getHistory();

// Invoke an action, dismiss, or manage Do Not Disturb:
// bro.sys.notifications.invokeAction(notifId, 'open-power');
// bro.sys.notifications.dismiss(notifId);
// bro.sys.notifications.close(notifId, 'dismissed');
// bro.sys.notifications.setDoNotDisturb(true);
// bro.sys.notifications.removeFromHistory(notifId);
// bro.sys.notifications.clearHistory();

// Events: 'posted' (alias 'notificationPosted'), 'closed' ('notificationClosed'),
//         'serverStatus', 'dndChanged'
bro.sys.notifications.on('posted', ({ notification, replaced, popupSuppressed }) => {
    console.log(`Notification [${notification.id}] ${notification.summary}: ${notification.body}`);
});

// ============================================================================
// 6. StatusNotifier / System Tray Host (`bro.sys.tray`)
// ============================================================================

bro.sys.tray.start({ becomeWatcher: true });
const trayStatus = bro.sys.tray.getStatus(); // { role, detail }

const trayItems = bro.sys.tray.getItems();
for (const item of trayItems) {
    // {
    //   id, appId, title, status: 'passive' | 'active' | 'needsAttention',
    //   category, icon: { name, themePath, hasPixmaps },
    //   overlayIcon, attentionIcon, tooltip: { icon, title, body },
    //   itemIsMenu, hasMenu, windowId, pid, hidden
    // }
    console.log(`Tray item: ${item.id} (${item.title}) status=${item.status}`);
}

// Interact with a tray item and its DBusMenu tree:
// bro.sys.tray.activate(item.id, 100, 20);
// bro.sys.tray.secondaryActivate(item.id, 100, 20);
// bro.sys.tray.contextMenu(item.id, 100, 20);
// bro.sys.tray.scroll(item.id, -1, 'vertical');
// const menuRoot = bro.sys.tray.getMenu(item.id);
// bro.sys.tray.menuAboutToShow(item.id, menuRoot.id);
// bro.sys.tray.menuEvent(item.id, childMenuId, 'clicked');

// Events: 'itemAdded', 'itemChanged', 'itemRemoved', 'itemDropped', 'menuChanged', 'hostStatus'
bro.sys.tray.on('itemAdded', (item) => {
    console.log('Tray item added:', item.id);
});

// ============================================================================
// 7. Process Table (`bro.sys.processes`)
// ============================================================================

const procs = bro.sys.processes.list(); // or bro.sys.listProcesses()
for (const p of procs.slice(0, 5)) {
    // { pid, ppid, uid, name, exe, cmdline, state, cpuPercent, rssBytes, vsizeBytes, threads, startTimeMs }
    console.log(`[${p.pid}] ${p.name} — CPU: ${p.cpuPercent.toFixed(1)}%, RSS: ${p.rssBytes} B`);
}

// Send a signal to a process (default SIGTERM = 15):
// bro.sys.processes.kill(pid, 15); // or bro.sys.killProcess(pid, 15)
