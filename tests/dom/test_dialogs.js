// Test Native Dialogs (alert, confirm, prompt)
// Exercises src/js/dialog_bindings.cpp

assert(typeof alert === "function", "alert global exists");
assert(typeof confirm === "function", "confirm global exists");
assert(typeof prompt === "function", "prompt global exists");
assert(typeof showOpenFileDialog === "function", "showOpenFileDialog exists");
assert(typeof showOpenFolderDialog === "function", "showOpenFolderDialog exists");
assert(typeof showSaveFileDialog === "function", "showSaveFileDialog exists");

assert(window.alert === alert, "window.alert matches global alert");
assert(window.confirm === confirm, "window.confirm matches global confirm");
assert(window.prompt === prompt, "window.prompt matches global prompt");
assert(window.showOpenFileDialog === showOpenFileDialog, "window.showOpenFileDialog matches");
assert(window.showOpenFolderDialog === showOpenFolderDialog, "window.showOpenFolderDialog matches");
assert(window.showSaveFileDialog === showSaveFileDialog, "window.showSaveFileDialog matches");

// 1. Default accept state in headless
assert(alert("Test alert message") === undefined, "alert returns undefined");
assert(confirm("Proceed with test?") === true, "confirm accepts by default in headless");
assert(prompt("Enter value", "default_val") === "default_val", "prompt returns default text when accepted");
assert(prompt("Enter value") === "", "prompt without default returns empty string");

// 2. setDialogAnswer(false) -> cancel branch
if (typeof setDialogAnswer === "function") {
    setDialogAnswer(false);
    assert(confirm("Should cancel") === false, "confirm returns false when setDialogAnswer(false)");
    assert(prompt("Should cancel", "ignored") === null, "prompt returns null when cancelled");

    // Restore accept
    setDialogAnswer(true);
    assert(confirm("Should accept") === true, "confirm returns true when setDialogAnswer(true)");
    assert(prompt("Should accept", "good") === "good", "prompt returns default when accept restored");
}

// 3. A filter SDL will not accept is an exception, not an empty list.
//
// The open/save dialogs themselves cannot be driven from a test — they are
// modal and native — but a refused filter is answered *before* anything opens,
// so this half is testable and is the half that used to be silent: the call
// came back with no files, which is exactly what a cancel looks like, and the
// application had no way to tell "you wrote a bad filter" from "somebody
// pressed Escape". `*` is only a pattern on its own, so `png*` is refused.
let refused = null;
try { showOpenFileDialog("Pictures|png*"); }
catch (e) { refused = e; }
assert(refused !== null, "an invalid filter pattern throws");
assert(/refused/.test(String(refused.message)), "the refusal says so: " + refused.message);

let saveRefused = null;
try { showSaveFileDialog("Pictures|png*"); }
catch (e) { saveRefused = e; }
assert(saveRefused !== null, "an invalid save filter pattern throws");
assert(/refused/.test(String(saveRefused.message)), "the save refusal says so: " + saveRefused.message);

// 4. Headless picking via setPickedFiles
if (typeof setPickedFiles === "function") {
    setPickedFiles(["/path/to/file1.png", "/path/to/file2.png"]);
    let picked = showOpenFileDialog("Images|png;jpg", true);
    assert(Array.isArray(picked) && picked.length === 2, "picked returns queued files");
    assert(picked[0] === "/path/to/file1.png" && picked[1] === "/path/to/file2.png", "picked files match queued");

    setPickedFiles(["/path/to/saved.png"]);
    let saved = showSaveFileDialog("Images|png");
    assert(saved === "/path/to/saved.png", "saved returns queued file");
}

// 5. Various argument types stringify safely
assert(confirm(12345) === true, "confirm with number argument");
assert(confirm(null) === true, "confirm with null argument");
assert(confirm(undefined) === true, "confirm with undefined argument");
assert(alert(999) === undefined, "alert with number argument");

console.log("test_dialogs: passed");
