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
// While blocked the engine continues pumping SDL events and ticking
// JS timers, so audio sequencers and existing animation timers keep
// running — but no further script runs until the dialog returns.
//
// Filter format is a single string: "Label|ext1;ext2;ext3"
//   "Audio Files|wav;mp3;ogg;flac"
//   "GLB / GLTF|glb;gltf"
//   "JSON|json"
// Pass an empty string (or omit) to show all files.
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
// defaultName seeds the filename field (engine treats it as a default
// location hint).

const path = showSaveFileDialog('WAV Files|wav', 'recording.wav');
if (path) {
    saveWav(path);
}


// -----------------------------------------------------------------------------
// Defensive use
// -----------------------------------------------------------------------------
//
// In headless / GPU-less contexts the globals may not be installed. App
// code commonly feature-tests before calling:

if (typeof showSaveFileDialog !== 'function') {
    throw new Error('Save dialog unavailable in this build');
}
