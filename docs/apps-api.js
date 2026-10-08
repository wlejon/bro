/**
 * @file docs/apps-api.js
 * @summary Documentation and examples for the `bro.apps` JavaScript API.
 */

/**
 * `bro.apps` provides application discovery, .desktop execution, icon lookup,
 * MIME type associations, and recent files management:
 * - Application catalog enumeration and searching (`list`, `get`, `search`)
 * - Scoped process launching with sandboxing/environment control (`launch`)
 * - FreeDesktop icon resolution with size/theme options (`resolveIcon`)
 * - MIME association querying (`getDefaultApp`, `getAppsForMime`)
 * - Recent files management (`getRecent`, `addRecent`, `clearRecent`)
 * - Live catalog watching (`watch`, `unwatch`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_APPS` is enabled.
 */

// ============================================================================
// 1. Application Discovery & Inspection
// ============================================================================

// List all registered desktop applications
const apps = bro.apps.list();
for (const app of apps) {
    console.log(`[${app.id}] ${app.name} (${app.genericName || 'No description'})`);
    console.log(`  Icon: ${app.icon}`);
    console.log(`  Exec: ${app.exec}`);
    console.log(`  Terminal: ${app.terminal}`);
    console.log(`  Categories: ${app.categories.join(', ')}`);
}

// Get specific application by desktop ID:
const editor = bro.apps.get('org.gnome.TextEditor.desktop');
if (editor) {
    console.log(`Found: ${editor.name}`);
}

// Search applications by query string (searches name, comment, keywords):
const browsers = bro.apps.search('web browser');

// Filter applications by category:
const devTools = bro.apps.findByCategory('Development');

// Filter applications by MIME type:
const pdfViewers = bro.apps.findByMimeType('application/pdf');

// ============================================================================
// 2. Application Launching (`bro.apps.launch`)
// ============================================================================

// Launch an application asynchronously with options:
const processHandle = await bro.apps.launch('org.gnome.TextEditor.desktop', {
    args: ['--new-window'],
    files: ['/path/to/file.txt'],
    cwd: '/path/to/project',
    env: { 'CUSTOM_VAR': '1' }
});

console.log(`Launched process PID: ${processHandle.pid()}`);
console.log(`Is running: ${processHandle.isRunning()}`);

// Monitor or control process lifecycle:
// processHandle.terminate(); // Graceful SIGTERM / WM_CLOSE
// processHandle.kill();      // SIGKILL / TerminateProcess
// const exitCode = await processHandle.wait();

// ============================================================================
// 3. Icon Resolution (`bro.apps.resolveIcon`)
// ============================================================================

// Resolve an icon name to a file path on disk (null when nothing matches).
// Follows the freedesktop Icon Theme Specification: the theme (default: the
// desktop's configured one: KDE kdeglobals, then GTK settings.ini, else hicolor),
// its Inherits chain, hicolor, then the unthemed dirs (~/.icons, <data>/icons,
// /usr/share/pixmaps). An exact size/scale match wins, otherwise the closest size.
// An absolute path is returned as is when the file exists.
//   size  - logical px (default 48); a bare number may be passed instead of options
//   scale - integer UI scale (default 1); 2 prefers 48x48@2 dirs on a 2x display
//   theme - theme directory name; unknown themes fall back to the configured one
const iconPath = bro.apps.resolveIcon('text-editor', {
    size: 48,
    scale: 1,
    theme: 'Adwaita'
});
console.log(`Resolved icon path: ${iconPath}`);

// ============================================================================
// 4. MIME Associations (`getDefaultApp`, `getAppsForMime`)
// ============================================================================

const defaultApp = bro.apps.getDefaultApp('text/plain');
if (defaultApp) {
    console.log(`Default text editor: ${defaultApp.name}`);
}

const allImageViewers = bro.apps.getAppsForMime('image/png');

// ============================================================================
// 5. Recent Items (`getRecent`, `addRecent`, `clearRecent`)
// ============================================================================

// Retrieve recently opened files
const recent = bro.apps.getRecent(10);
for (const item of recent) {
    console.log(`Recent: ${item.filePath} (app: ${item.appId}, at: ${item.timestamp})`);
}

// Record a recently used file:
bro.apps.addRecent('/path/to/project/main.cpp', 'org.gnome.TextEditor.desktop');

// Clear recent history:
// bro.apps.clearRecent();

// ============================================================================
// 6. Live Catalog Watching (`watch`, `unwatch`, `refresh`)
// ============================================================================

// Watch for desktop file additions, removals, and modifications:
const watcher = bro.apps.watch((event) => {
    console.log(`Catalog change: ${event.type} -> ${event.appId || event.path}`);
});

// Explicitly unwatch:
// watcher.unwatch();
// Or:
// bro.apps.unwatch(watcher.token);

// Force rescan of desktop directories:
bro.apps.refresh();
