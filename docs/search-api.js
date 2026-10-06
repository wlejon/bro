/**
 * @file docs/search-api.js
 * @summary Documentation and examples for the `bro.search` JavaScript API.
 */

/**
 * `bro.search` provides high-performance search primitives:
 * - Fzf-compatible fuzzy matching and incremental fuzzy indexing
 * - Ripgrep-style directory walking with .gitignore support
 * - Fast multi-threaded regex/literal text search (grep)
 * - Linear-time DFA/PikeVM regular expressions
 *
 * Mounted automatically in Bronze when `BRO_WITH_SEARCH` is enabled.
 */

// ============================================================================
// 1. Fuzzy Matching
// ============================================================================

// One-shot fuzzy filter on array of candidate strings or objects
const matches = bro.search.fuzzy('bld', [
    'build/Release/bro',
    'src/render/vulkan.cpp',
    'build.ninja',
    'docs/build-options.md'
]);
// returns array sorted by score descending:
// [
//   { item: 'build.ninja', score: 142, positions: [0, 1, 2] },
//   ...
// ]

// Fuzzy match with key selector on objects:
const objMatches = bro.search.fuzzy('term', [
    { title: 'Terminal', path: '/apps/term' },
    { title: 'Settings', path: '/apps/settings' }
], { key: item => item.title });

// Pre-indexed fuzzy search for repeated queries:
const index = new bro.search.FuzzyIndex([
    'file1.txt',
    'file2.cpp',
    'header.h'
]);
index.add('another_file.ts');
const results = index.search('file', { maxResults: 10 });

// ============================================================================
// 2. Directory File Walking (`bro.search.files`)
// ============================================================================

// Asynchronous directory walk respecting .gitignore rules:
const files = await bro.search.files('/home/j/projects/bro', {
    ignore: true,         // respect .gitignore
    hidden: false,        // skip hidden files
    maxDepth: 5,
    query: 'test'         // optional substring/pattern filter on path
});

// Synchronous directory walk:
const syncFiles = bro.search.filesSync('/home/j/projects/bro/tests', {
    maxDepth: 2
});

// Native Gitignore Matcher:
const gitignore = new bro.search.GitignoreMatcher('/home/j/projects/bro');
gitignore.addRules(['node_modules/', '*.o', 'build*/']);
const isIgnored = gitignore.isIgnored('build-release/bro'); // true

// ============================================================================
// 3. Grep (`bro.search.grep`)
// ============================================================================

// Search file contents across a directory tree:
const hits = await bro.search.grep('/home/j/projects/bro/src', 'installSiblingApis', {
    caseSensitive: true,
    maxResults: 50
});
// hits: [
//   { path: '/home/j/projects/bro/src/bronze_host/host_sibling_apis.cpp', line: 40, column: 5, text: 'void installSiblingApis(...)' }
// ]

// Streaming search with callback:
await bro.search.grep('/home/j/projects/bro/src', 'Engine', {
    onMatch: (hit) => {
        console.log(`${hit.path}:${hit.line} -> ${hit.text}`);
    }
});

// Linear-time Regex:
const regex = new bro.search.Regex('^bro_[a-z]+');
assert(regex.test('bro_vulkan_test')); // true
