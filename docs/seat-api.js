/**
 * @file docs/seat-api.js
 * @summary Documentation and examples for the `bro.seat` JavaScript API.
 */

/**
 * `bro.seat` provides session state inspection, lock/unlock control, virtual terminal
 * switching, idle/sleep/shutdown inhibition, and XDG autostart discovery and launching:
 * - Session state query (`getSessionState`)
 * - Screen lock and unlock control (`lock`, `unlock`)
 * - Virtual terminal switching (`switchVt`)
 * - Idle / sleep / shutdown inhibition handles (`inhibit`, `uninhibit`, `listInhibitors`)
 * - XDG Autostart management (`listAutostart`, `runAutostart`)
 * - Session event listening (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_SEAT` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["seat", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Session State & VT Switching
// ============================================================================

if (bro.seat.available) {
    const session = bro.seat.getSessionState();
    console.log('Session state:', {
        active: session.active,
        locked: session.locked,
        vt: session.vt,
        id: session.id,
        user: session.user,
        seat: session.seat
    });

    // Listen for session lock and unlock events
    bro.seat.on('lock', () => {
        console.log('Session has been locked');
    });

    bro.seat.on('unlock', () => {
        console.log('Session has been unlocked');
    });

    // Switch to virtual terminal 1
    // bro.seat.switchVt(1);
}

// ============================================================================
// 2. Idle and Sleep Inhibition
// ============================================================================

// Take an idle inhibitor (e.g. during video playback or presentation)
const inhibitId = bro.seat.inhibit('idle', 'Playing full-screen presentation');
console.log(`Acquired inhibitor ID: ${inhibitId}`);

// List active inhibitors:
const inhibitors = bro.seat.listInhibitors();
for (const inh of inhibitors) {
    console.log(`Inhibitor #${inh.id} [${inh.type}]: ${inh.reason}`);
}

// Release inhibitor:
if (inhibitId > 0) {
    bro.seat.uninhibit(inhibitId);
}

// ============================================================================
// 3. XDG Autostart Management
// ============================================================================

// Discover autostart desktop entries
const autostartEntries = bro.seat.listAutostart();
for (const entry of autostartEntries) {
    console.log(`Autostart: ${entry.name} (${entry.id}) - exec: ${entry.exec}, enabled: ${entry.enabled}`);
}

// Launch all enabled autostart entries asynchronously:
bro.seat.runAutostart().then(results => {
    for (const res of results) {
        console.log(`Launched ${res.id}: PID ${res.pid}, success=${res.success}`);
    }
});
