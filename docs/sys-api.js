/**
 * @file docs/sys-api.js
 * @summary Documentation and examples for the `bro.sys` JavaScript API.
 */

/**
 * `bro.sys` provides desktop system daemon and hardware state integration across
 * 9 sub-namespaces:
 * - `bro.sys.power`: Battery/AC state, power actions (suspend/reboot/powerOff), and inhibitors
 * - `bro.sys.audio`: PipeWire / PulseAudio device enumeration, volume, mute, and default routing
 * - `bro.sys.network`: NetworkManager / OS interfaces, Wi-Fi scanning/connecting, and VPN control
 * - `bro.sys.bluetooth`: BlueZ adapter control, discovery, pairing, and device connection
 * - `bro.sys.notifications`: Desktop notification daemon (`org.freedesktop.Notifications`), history, and DND
 * - `bro.sys.tray`: StatusNotifierWatcher / system tray host, item activation, and DBusMenu inspection
 * - `bro.sys.processes`: Process table inspection and signaling (`list`, `kill`, plus top-level aliases)
 * - `bro.sys.journal`: the systemd journal (Linux): structured queries, backward paging, live follow
 * - `bro.sys.disks`: UDisks2 drives and block devices (Linux): mount, unmount, eject, power off, SMART
 *
 * Gated member by member (docs/apps.md, Permissions): each of `bro.sys.power`, `.audio`,
 * `.network`, `.bluetooth`, `.notifications`, `.tray`, `.processes`, `.journal` and `.disks` needs its own
 * capability, `sys.<member>` (`sys.processes` also covers the top-level `listProcesses` /
 * `killProcess`). A member the app declares is asked for at first touch; until granted it is
 * the unavailable stub (`available === false`, `reason`, every call throws), while the
 * granted members are live. Listing processes therefore never brings power-off with it.
 * Compiled in with `BRO_WITH_SYS`.
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
    // { pid, ppid, name, cmdline, state, user, uid, rssBytes, vszBytes, cpuTimeMs, threads }
    // (cpuTimeMs is cumulative: sample twice for a CPU percentage)
    console.log(`[${p.pid}] ${p.name} — CPU time: ${p.cpuTimeMs} ms, RSS: ${p.rssBytes} B`);
}

// Send a signal to a process (default SIGTERM = 15):
// bro.sys.processes.kill(pid, 15); // or bro.sys.killProcess(pid, 15)

// ============================================================================
// 8. The systemd journal (`bro.sys.journal`, Linux; capability sys.journal)
// ============================================================================
//
// Read through libsystemd's sd_journal, never journalctl: the system journal
// and the user's own, as far as the user may read them (the systemd-journal,
// adm or wheel group sees every service). Every query runs on a thread of its
// own and returns a Promise; a follow runs on its own thread and calls back on
// the page's thread. Off Linux `available` is false and `reason` says why.
//
// JournalQueryOptions (every key optional):
//   unit / units   'sshd.service' or an array: records of any of these system or user units
//   priority       0..7 or 'emerg' 'alert' 'crit' 'err' 'warning' 'notice' 'info' 'debug':
//                  that priority and more severe
//   boot           'current' / 0 this boot, -1 / -2 ... the boots before it, or a boot id
//   since, until   ms since the epoch, or a Date
//   match          { FIELD: value | [values] }: one field's values are alternatives,
//                  different fields must all hold ({ _COMM: 'sshd', _UID: 1000 })
//   text           case-insensitive substring of MESSAGE
//   before         a cursor: only records older than it (the next page back)
//   after          follow only: start after this cursor (else new records only)
//   limit          records per page (default 200)
//   allFields      add `fields`, every field of each record
//
// JournalEntry:
//   { cursor, time (ms since the epoch), realtimeUs, monotonicUs, bootId,
//     priority (0..7 or null), message, unit, identifier (SYSLOG_IDENTIFIER or _COMM),
//     pid, hostname, transport ('journal' | 'syslog' | 'kernel' | 'stdout' | ...),
//     fields? ({ FIELD: value }, with allFields) }

// The newest 100 warnings and worse of this boot, oldest first:
const page = await bro.sys.journal.query({ boot: 'current', priority: 'warning', limit: 100 });
// page = { entries: JournalEntry[], before: cursor | null, more: boolean }
for (const e of page.entries) console.log(new Date(e.time), e.unit || e.identifier, e.message);

// The page before it (scrolling back): pass `before`.
if (page.more) {
    const older = await bro.sys.journal.query({ boot: 'current', priority: 'warning', limit: 100,
                                                before: page.before });
}

// Live, from the newest record already shown, so nothing is missed between the
// query and the follow:
const newest = page.entries.at(-1);
const follow = bro.sys.journal.follow(
    { boot: 'current', priority: 'warning', after: newest ? newest.cursor : undefined,
      onError: (err) => console.log('follow ended:', err.message) },
    (entries) => { for (const e of entries) console.log(e.message); });   // batches, oldest first
// follow = { id, close() }
follow.close();

// The boots in the journal, oldest first: [{ id, offset (0 = newest), first, last (ms) }]
const boots = await bro.sys.journal.boots();

// The distinct values of a field, sorted: the units that logged, say.
const units = await bro.sys.journal.values('_SYSTEMD_UNIT');

bro.sys.journal.priorities;   // ['emerg', 'alert', 'crit', 'err', 'warning', 'notice', 'info', 'debug']

// ============================================================================
// 9. Disks (`bro.sys.disks`, Linux; capability sys.disks)
// ============================================================================
//
// UDisks2 over D-Bus, never udisksctl or smartctl: the drives and block devices
// the daemon sees, mount/unmount, eject and power off, and SMART health. polkit
// decides who may act: the active local session usually may mount, unmount and
// eject removable media; a call it refuses rejects with polkit's message (and
// a polkit agent, where one runs, may ask the user first). Snapshots are
// synchronous; actions and SMART reads return Promises. Off Linux, or without a
// UDisks2 daemon, `available` is false and `reason` says why.
//
// Drive:
//   { id (UDisks object path), vendor, model, serial, revision, wwn, size (bytes),
//     connectionBus ('usb' | 'sdio' | 'ieee1394' | '' internal), media, rotationRate
//     (rpm; 0 non-rotating, -1 unknown), removable, mediaRemovable, mediaAvailable,
//     ejectable, canPowerOff, optical, seat, sortKey,
//     smartKind ('nvme' | 'ata' | null), smartSupported }
// Block:
//   { id, device ('/dev/sda1'), preferredDevice, drive (Drive id or ''), size, readOnly,
//     usage ('filesystem' | 'crypto' | 'raid' | 'other' | ''), type ('ext4', 'vfat', ...),
//     label, uuid, hintSystem, hintIgnore, hintAuto, hintName, hintIcon,
//     partition ({ number, table (Block id), type, name, uuid, offset } or null),
//     partitionTable ('gpt' | 'dos' | ''), isFilesystem, mountPoints (string[]), mounted,
//     loopBackingFile, cryptoBackingDevice }
// Smart:
//   { drive, kind, supported, enabled, updated (ms or null), failing,
//     assessment ('good' | 'warning' | 'failing' | 'unknown'),
//     temperatureC, powerOnHours, percentUsed (NVMe wear), availableSpare, spareThreshold,
//     badSectors (ATA: UDisks' bad-sector count), powerCycles, unsafeShutdowns, mediaErrors,
//     bytesRead, bytesWritten (each a number or null where the drive does not say),
//     criticalWarnings (NVMe: 'spare', 'temperature', 'degraded', 'readonly', ...),
//     selftestStatus, attributes: [{ id, name, value, worst, threshold, raw, unit, failing }] }

const disks = bro.sys.disks;
const { available, reason, drives, blocks } = disks.getState();
disks.getDrives();                       // Drive[]
disks.getBlocks();                       // Block[]
disks.getDrive(drives[0].id);            // Drive or null
const stick = disks.getBlock('/dev/sdb1');   // by device path or id; Block or null

// Mount a filesystem; resolves with where it went (/run/media/<user>/<label>):
const where = await disks.mount(stick.id, { options: 'ro' });   // { fstype, options } optional
await disks.unmount(stick.id);                 // { force: true } for a lazy unmount
// Eject the medium (optical, card readers) or power the drive off (USB): both
// unmount the drive's mounted filesystems first.
await disks.eject(stick.drive);
await disks.powerOff(stick.drive);

// SMART: NVMe health log or ATA attributes, read without privileges where the
// daemon allows (it caches; `updated` says when it last read the drive).
const health = await disks.smart(drives[0].id);
console.log(health.assessment, health.temperatureC, health.powerOnHours, health.percentUsed);

// Changes, as UDisks reports them: driveAdded / driveChanged (Drive), driveRemoved ({ id }),
// blockAdded / blockChanged (Block), blockRemoved ({ id }), mounted / unmounted /
// mountChanged ({ block, mountPoints, previous }), availability ({ available }); and
// 'change' after each one: { type, drive | block | id | available }.
const onChange = (c) => console.log(c.type);
disks.on('change', onChange);
disks.off('change', onChange);
