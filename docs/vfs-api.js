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
 * - MIME type detection and volume listing (`getMime`, `volumes`, `listVolumes`)
 * - Disk usage of a whole tree, live while it is counted (`usage`)
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

// bro.vfs.volumes() answers the same list off the calling thread. Asking a
// volume for its size can take seconds (a sleeping disk, a network share that
// has gone away), so a window lists drives with this, never with the
// synchronous listVolumes() on its way to the first frame.
const volumes = await bro.vfs.volumes();      // or bro.vfs.listVolumes(), synchronous
for (const vol of volumes) {
    // { mountPoint, volumeLabel, fsType, totalBytes, freeBytes, availableBytes,
    //   isReadOnly, isRemovable, isNetwork, usedPercentage }
    console.log(`Mount: ${vol.mountPoint} ${vol.volumeLabel} (${vol.fsType})`);
    console.log(`  Total: ${vol.totalBytes} bytes, Available: ${vol.availableBytes} bytes`);
}

// ============================================================================
// 7. Disk Usage of a Whole Tree (`bro.vfs.usage`)
// ============================================================================
//
// Totals a tree in the background, on a few threads of its own, while the
// page reads partial results: every directory's bytes / files / directories
// cover its whole subtree and grow as the scan counts its descendants. The
// page never holds the tree; it asks for the one folder it shows. A drive of
// a million files takes seconds and costs the page nothing but its reads.
// Links are counted, never followed; sizes are logical (bytes held, not
// clusters). Hidden: a dot name on POSIX, the hidden attribute on Windows.

const scan = bro.vfs.usage('/home/j', { includeHidden: false, threads: 4 });  // running already
scan.root;                       // '/home/j'
scan.done.then((p) => console.log(`done in ${p.elapsedMs} ms`));  // settles when finished or cancelled

// Poll from requestAnimationFrame; redraw only when `version` moved.
const p = scan.progress();
// { files, directories, bytes, errors, version, finished, cancelled, elapsedMs }

// The children of a folder (the root when omitted), biggest first, cut to
// `limit` natively; null until the scan has reached that folder.
const top = scan.children('/home/j', { sort: 'bytes', limit: 500 });   // sort: 'bytes' | 'name'
for (const it of top || []) {
    // { name, path, kind, isDirectory, bytes, files, directories, children,
    //   mtime, hidden, complete }   complete: its whole subtree is counted
    console.log(`${it.name}: ${it.bytes} bytes${it.complete ? '' : ' so far'}`);
}

scan.entry('/home/j/Videos');    // one item (the root when omitted), or null
scan.remove('/home/j/Videos/old.mkv');  // after trashing it: its bytes come off every folder above; false if unknown
scan.errors();                   // [{ path, message, code }], the first 100
scan.cancel();                   // stops; what was counted stays readable
