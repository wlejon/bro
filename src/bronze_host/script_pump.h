#pragma once

// The script watchdog: keeps the page thread answering through a long
// stretch of script at boot (util/page_pump.h).
//
// While the boot's pump watch runs (from the window to the first frame), a
// thread of its own looks at the page thread's last pump every few ms; once
// it is kPumpIntervalMs old, it requests a bronze host interrupt on the page
// thread (embed::requestHostInterrupt), so the next loop backedge or call of
// the script running there pumps (PumpReason::Interrupt: the window's
// messages processed and left queued, nothing drawn, no script run). Natives
// are not interrupted; those that block do their own pumping (waitPumping).

namespace bro::bronze_host {

// On the page thread. Idempotent; a no-op once the pump watch has stopped.
void startScriptPumpWatchdog();
// On the page thread: stops it and waits for its thread.
void stopScriptPumpWatchdog();

}  // namespace bro::bronze_host
