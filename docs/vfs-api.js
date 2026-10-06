/**
 * @file docs/vfs-api.js
 * @summary Documentation and examples for the `bro.vfs` JavaScript API.
 */

/**
 * `bro.vfs` provides high-performance asynchronous filesystem operations,
 * directory scanning, XDG trash management with multi-level undo/redo,
 * filesystem watching, MIME detection, and volume inspection:
 * - Recursive and filtered directory scanning (`scan`)
 * - Asynchronous copy, move, and remove with journaled undo (`copy`, `move`, `remove`, `undo`, `redo`)
 * - Freedesktop / OS trash specification (`trash`, `restoreTrash`, `listTrash`, `emptyTrash`)
 * - Reactive directory models with sorting and filtering (`DirectoryModel`)
 * - File and directory change watching (`watch`)
 * - MIME type detection and volume listing (`getMime`, `listVolumes`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_VFS` is enabled.
 */

// ============================================================================
// 1. Directory Scanning (`bro.vfs.scan`)
// ============================================================================

const entries = await bro.vfs.scan('/home/j/projects', {
    recursive: false,
    includeHidden: false
});

for (const entry of entries) {
    console.log(`${entry.name} - ${entry.isDirectory ? 'DIR' : 'FILE'} (${entry.size} bytes)`);
    console.log(`  Path: ${entry.path}`);
    console.log(`  Modified: ${entry.mtime}`);
}

// ============================================================================
// 2. File Operations with Undo Journal (`copy`, `move`, `remove`, `undo`, `redo`)
// ============================================================================

// Asynchronously copy file or directory:
const copyRes = await bro.vfs.copy('/path/to/source.txt', '/path/to/backup.txt');
console.log(`Copied: ${copyRes.ok}`);

// Move/rename:
const moveRes = await bro.vfs.move('/path/to/backup.txt', '/path/to/dest.txt');

// Undo the last operation:
if (bro.vfs.canUndo()) {
    const undone = await bro.vfs.undo();
    console.log(`Undo succeeded: ${undone}`);
}

// Redo:
if (bro.vfs.canRedo()) {
    const redone = await bro.vfs.redo();
    console.log(`Redo succeeded: ${redone}`);
}

// Remove:
await bro.vfs.remove('/path/to/temp_dir', { recursive: true });

// ============================================================================
// 3. Trash Management (`trash`, `restoreTrash`, `listTrash`, `emptyTrash`)
// ============================================================================

// Send file to XDG / OS trash:
const trashItem = await bro.vfs.trash('/path/to/unwanted.txt');
console.log(`Trashed to ID: ${trashItem.id} (original path: ${trashItem.originalPath})`);

// List trash contents:
const trashList = await bro.vfs.listTrash();
for (const item of trashList) {
    console.log(`Trash item: ${item.id} -> ${item.originalPath} (deleted at: ${item.deletionDate})`);
}

// Restore an item from trash:
const restored = await bro.vfs.restoreTrash(trashItem.id);

// Permanently empty trash:
// await bro.vfs.emptyTrash();

// ============================================================================
// 4. Reactive Directory Model (`bro.vfs.DirectoryModel`)
// ============================================================================

const model = new bro.vfs.DirectoryModel('/home/j/projects/bro');

// Listen for updates:
model.on('change', () => {
    console.log(`Directory updated, count: ${model.entries().length}`);
});

// Configure sorting and filtering:
model.setSort('name', true);       // sort by name ascending
model.setFilter('*.cpp');          // filter by glob or substring
model.refresh();

const currentItems = model.entries();

// ============================================================================
// 5. Filesystem Watching (`bro.vfs.watch`)
// ============================================================================

const watcher = bro.vfs.watch('/home/j/projects/bro', (events) => {
    for (const ev of events) {
        console.log(`Watch event: ${ev.type} on ${ev.path}`);
    }
}, { recursive: true });

// Stop watching:
// watcher.unwatch();

// ============================================================================
// 6. MIME Type & Volume Inspection
// ============================================================================

const mime = bro.vfs.getMime('/home/j/projects/bro/README.md');
console.log(`MIME type: ${mime}`); // e.g. text/markdown

const volumes = bro.vfs.listVolumes();
for (const vol of volumes) {
    console.log(`Mount: ${vol.mountPoint} (${vol.filesystemType})`);
    console.log(`  Total: ${vol.totalBytes} bytes, Available: ${vol.availableBytes} bytes`);
}
