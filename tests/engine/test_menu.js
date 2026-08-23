// Test bro.menu in engine test suite
// Exercises src/js/menu_bindings.cpp and src/engine/menu_bar.cpp

assert(typeof bro === "object", "bro exists");
assert(typeof bro.menu === "object", "bro.menu exists");

// 1. show / hide / visible
bro.menu.hide();
assert(bro.menu.visible === false, "visible false after hide()");

bro.menu.show();
assert(bro.menu.visible === true, "visible true after show()");

bro.menu.hide();
assert(bro.menu.visible === false, "visible false after hide()");

// 2. set hierarchy
bro.menu.set([
    { id: "file", label: "File", items: [
        { id: "file.new", label: "New", accel: "Ctrl+N" },
        { id: "file.open", label: "Open", accel: "Ctrl+O" },
        { separator: true },
        { id: "file.save", label: "Save", enabled: false },
        { id: "__system.quit", label: "Quit" },
    ]},
    { id: "edit", label: "Edit", items: [
        { id: "edit.undo", label: "Undo" },
        { id: "edit.redo", label: "Redo" },
    ]},
]);

// 3. addItem to submenu and root
const addedSub = bro.menu.addItem("file", {
    id: "file.export",
    label: "Export",
    items: [
        { id: "export.png", label: "PNG Image" },
        { id: "export.pdf", label: "PDF Document" },
    ]
}, 2);
assert(addedSub === true, "addItem to submenu succeeded");

const addedRoot = bro.menu.addItem("", {
    id: "help",
    label: "Help",
    items: [
        { id: "help.about", label: "About" },
    ]
});
assert(addedRoot === true, "addItem to root succeeded");

// 4. updateItem
assert(bro.menu.updateItem("file.save", { enabled: true }) === true, "updateItem enabled succeeded");
assert(bro.menu.updateItem("export.png", { label: "PNG Image (*.png)" }) === true, "updateItem label succeeded");
assert(bro.menu.updateItem("help.about", { checked: true }) === true, "updateItem checked succeeded");
assert(bro.menu.updateItem("help.about", { hidden: true }) === true, "updateItem hidden succeeded");

// 5. removeItem
assert(bro.menu.removeItem("export.pdf") === true, "removeItem succeeded");
assert(bro.menu.removeItem("edit.redo") === true, "removeItem redo succeeded");

// 6. on() handler
let clickCount = 0;
bro.menu.on("file.new", () => { clickCount++; });
bro.menu.on("file.open", () => {});

// 7. Error cases and clearing
assert(bro.menu.removeItem("nonexistent_item_id") === false, "removing nonexistent item returns false");
assert(bro.menu.updateItem("nonexistent_item_id", { label: "none" }) === false, "updating nonexistent item returns false");

// Clear menu
bro.menu.set([]);

console.log("test_menu: passed");
