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

// 3. Various argument types stringify safely
assert(confirm(12345) === true, "confirm with number argument");
assert(confirm(null) === true, "confirm with null argument");
assert(confirm(undefined) === true, "confirm with undefined argument");
assert(alert(999) === undefined, "alert with number argument");

console.log("test_dialogs: passed");
