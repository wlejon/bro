/**
 * @file docs/thumb-api.js
 * @summary Documentation and examples for the `bro.thumb` JavaScript API.
 */

/**
 * `bro.thumb` provides asynchronous and synchronous thumbnail generation,
 * caching according to Freedesktop Thumbnail Managing Standards, failure tracking,
 * and capability discovery:
 * - Asynchronous thumbnail requests (`get`) returning Promises with image pixel data
 * - Synchronous thumbnail retrieval (`getSync`)
 * - Cache invalidation and purging (`invalidate`, `clearCache`)
 * - Expected cache path lookups (`getThumbnailPath`)
 * - Failure recording, checking, and clearing (`hasFailed`, `recordFailure`, `clearFailure`)
 * - Platform generator capabilities (`getCapabilities`)
 * - Base cache directory discovery (`getBaseDir`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_THUMB` is enabled.
 *
 * The cache lives in the platform's thumbnail directory
 * (`$XDG_CACHE_HOME/thumbnails`, `%LOCALAPPDATA%\thumbnails`), except under
 * `BRO_APP_HOME` (a test's or a scratch profile's home), where it is
 * `<BRO_APP_HOME>/cache/thumbnails`: `getBaseDir()`, `getThumbnailPath()`
 * and every generated file follow it, so a test never writes into the
 * user's real cache.
 */

// ============================================================================
// 1. Availability & Capabilities
// ============================================================================

if (bro.thumb.available) {
    const caps = bro.thumb.getCapabilities();
    console.log('Supported extensions:', caps.supportedExtensions);
    console.log('Supported MIME types:', caps.supportedMimeTypes);
    console.log('Capabilities:', {
        canExtractImages: caps.canExtractImages,
        canExtractPdfs: caps.canExtractPdfs,
        canExtractText: caps.canExtractText,
        canExtractNative: caps.canExtractNative,
    });
}

// ============================================================================
// 2. Asynchronous Thumbnail Generation (`bro.thumb.get`)
// ============================================================================

// Request thumbnail asynchronously (returns a Promise)
bro.thumb.get('/path/to/image.png', {
    size: 'normal',       // 'normal' (128), 'large' (256), 'x-large' (512), 'xx-large' (1024), or pixel number
    priority: 'normal',   // 'low', 'normal', 'high'
    cacheOnly: false,     // only return if already in cache
    force: false,         // regenerate even if cached
    preferNative: false   // prefer native OS thumbnailer if available
}).then(result => {
    console.log(`Thumbnail width: ${result.width}, height: ${result.height}`);
    console.log(`Thumbnail source: ${result.source}`); // 'cache', 'generator', 'native', etc.
    console.log(`Cached thumbnail path: ${result.path}`);
    console.log(`RGBA pixels Uint8Array: ${result.pixels.length} bytes`);
}).catch(err => {
    console.error(`Failed to generate thumbnail: ${err.message}`);
});

// ============================================================================
// 3. Synchronous Thumbnail Lookup (`bro.thumb.getSync`)
// ============================================================================

const syncResult = bro.thumb.getSync('/path/to/image.png', { size: 'normal' });
if (syncResult) {
    console.log(`Synchronous thumbnail ready: ${syncResult.width}x${syncResult.height}`);
} else {
    console.log('Thumbnail not available synchronously or generation failed');
}

// ============================================================================
// 4. Cache Management & Failure Handling
// ============================================================================

// Expected Freedesktop thumbnail path on disk:
const thumbPath = bro.thumb.getThumbnailPath('/path/to/image.png', { size: 'large' });
console.log(`Expected cache path: ${thumbPath}`);

// Invalidate cached thumbnails for a source file across all size tiers:
bro.thumb.invalidate('/path/to/image.png');

// Check and record permanent failure state:
if (!bro.thumb.hasFailed('/path/to/corrupt.pdf')) {
    // Record that generation failed for this file
    bro.thumb.recordFailure('/path/to/corrupt.pdf');
}

// Clear recorded failure:
bro.thumb.clearFailure('/path/to/corrupt.pdf');

// Clear thumbnail cache:
bro.thumb.clearCache(); // clears entire cache
