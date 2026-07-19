// =============================================================================
// bro Native File Dialogs API
// =============================================================================
//
// Native open/save dialogs backed by SDL3's portable file dialog API
// (Win32 IFileDialog, GTK/portal on Linux, NSOpenPanel on macOS). These
// are *globals*, not on the bro.* namespace, and they predate brokit's
// own showOpenFilePicker — apps in this repo use these, not the
// web-standard variants.
//
// All three calls block the JS thread until the user picks or cancels.
// While blocked the engine spins an 8 ms loop that pumps SDL events and
// runs the JS timer queue (setTimeout / setInterval), so an audio
// sequencer driven by timers keeps going. That is ALL that runs:
// requestAnimationFrame callbacks, layout, and rendering are stopped for
// the whole life of the dialog, so the window does not repaint and CSS
// animations do not advance. No further script runs until the call returns.
//
// All three are declared with arity 0, so `showOpenFileDialog.length === 0`
// despite the signatures below — arguments are read positionally at runtime.
//
// Filter format is a single string: "Label|ext1;ext2;ext3"
//   "Audio Files|wav;mp3;ogg;flac"
//   "GLB / GLTF|glb;gltf"
//   "JSON|json"
// Pass an empty string (or omit) to show all files. A non-string filter
// argument is silently ignored (same as omitting it). A string with no "|"
// is taken as the PATTERN, with the literal label "Files" — so 'json' works
// but is displayed as "Files".
// =============================================================================


// -----------------------------------------------------------------------------
// showOpenFileDialog(filter?, allowMultiple?) → string[]
// -----------------------------------------------------------------------------
//
// Returns an array of absolute file paths. Empty array if the user
// cancelled. With allowMultiple = false (default) the array has 0 or 1
// entries; with true it can have many.

const files = showOpenFileDialog('Audio Files|wav;flac;mp3;ogg;opus');
if (files.length) {
    loadAudio(files[0]);
}

// Multiple selection:
const many = showOpenFileDialog('Images|png;jpg;jpeg', true);
many.forEach(loadImage);


// -----------------------------------------------------------------------------
// showOpenFolderDialog(defaultLocation?, allowMultiple?) → string[]
// -----------------------------------------------------------------------------
//
// defaultLocation is an optional starting directory (absolute path).
// Returns an array of folder paths; empty if cancelled.

const dirs = showOpenFolderDialog(state.lastDir || null);
if (dirs.length) {
    state.lastDir = dirs[0];
    openProjectFolder(dirs[0]);
}


// -----------------------------------------------------------------------------
// showSaveFileDialog(filter?, defaultName?) → string | null
// -----------------------------------------------------------------------------
//
// Returns a single absolute path, or null if the user cancelled.
//
// defaultName is NOT a filename hint: it is handed to SDL as the default
// *location* (after backslash normalization on Windows), so a bare
// 'recording.wav' is interpreted as a path, not as a pre-filled name. Pass a
// full path — 'C:\\Users\\me\\Music\\recording.wav' or `${dir}/recording.wav`
// — if you want the dialog to open somewhere specific.

const path = showSaveFileDialog('WAV Files|wav', `${state.lastDir}/recording.wav`);
if (path) {
    saveWav(path);
}


// -----------------------------------------------------------------------------
// Where these globals exist — and why the feature test matters
// -----------------------------------------------------------------------------
//
// They are installed unconditionally on the PRIMARY app realm, headless
// included. Headless is NOT a safe harbour: with no window the dialogs are
// installed against a null parent window and still enter the same blocking
// wait, so calling one from a test hangs the run until a native dialog is
// dismissed by hand. Never trigger a dialog from a headless test — gate it
// behind an explicit user action, or behind your own flag.
//
// The globals are genuinely ABSENT in <iframe> sub-documents and in
// secondary windows (bro.window.open) — those realms are built without the
// dialog bindings. That is what the feature test protects against: code that
// may run in a sub-document, not code that may run headless.

if (typeof showSaveFileDialog !== 'function') {
    // Reached from an <iframe> or a secondary window — ask the host realm to
    // run the dialog and post the path back.
    throw new Error('Save dialog is available in the main app realm only');
}
