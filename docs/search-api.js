/**
 * @file docs/search-api.js
 * @summary Documentation and examples for the `bro.search` JavaScript API.
 *
 * A walk's or grep's root, a GitignoreMatcher's root and a loaded ignore file
 * are read as `fs` would read them (docs/apps.md, Permissions); a refusal is
 * an Error carrying the reason, `code: 'EACCES'` and `path`. A files() or
 * grep() still running when the user takes the grant back is cancelled,
 * streams no more onFile/onMatch calls, and its promise rejects with an Error
 * named PermissionRevoked (`code: 'EACCES'`, `path` the root).
 */

/**
 * `bro.search` provides fzf-style fuzzy matching, persistent fuzzy indices,
 * gitignore-aware parallel file walking, ripgrep-style content search, and
 * linear-time regular expressions:
 * - One-shot fuzzy matching and scoring (`fuzzy`, `fuzzyMatch`)
 * - Persistent in-memory fuzzy index (`FuzzyIndex`)
 * - Parallel gitignore-aware file discovery (`files`, `filesSync`, `GitignoreMatcher`)
 * - Parallel regex/literal file and buffer search (`grep`, `grepSync`, `grepBuffer`)
 * - Linear-time regex engine (`Regex`, `regex`) and cooperative cancellation (`CancellationToken`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_SEARCH` is enabled.
 */

console.log('bro.search version:', bro.search.version);

// ============================================================================
// 1. One-Shot Fuzzy Matching (`fuzzy`, `fuzzyMatch`)
// ============================================================================

const candidates = [
    'src/core/engine.cpp',
    'src/core/renderer.cpp',
    'src/ui/window.cpp',
    'docs/search-api.js'
];

// Rank and filter candidates (returns [{ item, score, index, positions? }]):
const results = bro.search.fuzzy('srcrend', candidates, {
    limit: 10,
    withPositions: true,
    case: 'smart',       // 'ignore' | 'respect' | 'smart' (or `caseSensitive: boolean`)
    scheme: 'path',      // 'default' | 'path' | 'history'
    algo: 'v2',          // 'v1' | 'v2'
    extended: true,      // fzf extended-search syntax (`'exact`, `^prefix`, `suffix$`, `!neg`)
    normalize: true,     // fold diacritics
    sort: true
});

for (const r of results) {
    console.log(`${r.item} (score: ${r.score}, matched indices: ${r.positions?.join(',')})`);
}

// Score a single candidate against a query (returns `{ score, positions } | null`):
const singleMatch = bro.search.fuzzyMatch('win', 'src/ui/window.cpp', { scheme: 'path' });

// ============================================================================
// 2. Persistent Fuzzy Index (`bro.search.FuzzyIndex`)
// ============================================================================

const index = new bro.search.FuzzyIndex(candidates, {
    scheme: 'path',
    case: 'smart',
    algo: 'v2'
});

index.add('src/ui/widget.cpp');
index.add(['src/audio/mixer.cpp', 'src/audio/synth.cpp']);
console.log('Indexed item count:', index.size, 'item[0]:', index.item(0));

const hits = index.search('audio', { limit: 5, withPositions: true });
for (const hit of hits) {
    console.log(`Index hit: ${hit.item} (score: ${hit.score})`);
}

// Manage query cache or reset index:
index.setCacheEnabled(true);
index.clearCache();
// index.clear();

// ============================================================================
// 3. Parallel File Walking (`files`, `filesSync`, `GitignoreMatcher`)
// ============================================================================

const token = new bro.search.CancellationToken();

// Asynchronously walk a directory tree respecting .gitignore:
const files = await bro.search.files('/home/j/projects/bro', {
    ignore: true,           // respect .gitignore / .ignore
    requireGit: false,      // apply .gitignore even outside a git repo
    hidden: false,          // skip dot-files/directories
    followSymlinks: false,
    yieldDirectories: false,
    maxDepth: 8,
    globs: ['*.cpp', '*.h', '!build/**'],
    query: 'window',        // optional fuzzy filter applied during walk
    maxResults: 200,
    cancellationToken: token,
    onFile: (path) => {
        // Optional streaming callback invoked as files are discovered
    }
});
console.log(`Found ${files.length} matching files`);

// Synchronous variant:
const syncFiles = bro.search.filesSync('/home/j/projects/bro/docs', {
    globs: ['*-api.js']
});

// Standalone .gitignore rule matcher (`GitignoreMatcher`, alias `Gitignore`):
const matcher = new bro.search.GitignoreMatcher('/home/j/projects/bro', {
    caseMode: 'auto' // 'auto' | 'sensitive' | 'insensitive'
});
matcher.addRules('*.log\n!important.log\nbuild/\n');
console.log('ignored:', matcher.isIgnored('debug.log'));       // true
console.log('match kind:', matcher.match('important.log'));    // 'whitelist' | 'ignore' | 'none'

// ============================================================================
// 4. Content Search (`grep`, `grepSync`, `grepBuffer`)
// ============================================================================

// Asynchronous parallel grep across a directory tree:
const grepHits = await bro.search.grep('/home/j/projects/bro/src', 'registerModule', {
    fixedStrings: true,     // literal string search (false = regex)
    smartCase: true,
    caseSensitive: false,
    word: false,
    wholeLine: false,
    invert: false,
    multiline: false,
    maxFilesize: 10 * 1024 * 1024,
    ignore: true,
    hidden: false,
    globs: ['*.cpp', '*.h'],
    maxResults: 500
});

for (const hit of grepHits) {
    // { path, line, column, text } (line and column are 1-based)
    console.log(`${hit.path}:${hit.line}:${hit.column}: ${hit.text}`);
}

// Synchronous directory grep:
const syncHits = bro.search.grepSync('/home/j/projects/bro/docs', 'bro\\.search', {
    globs: ['search-api.js']
});

// In-memory buffer grep (accepts string, ArrayBuffer, or Uint8Array):
const bufHits = bro.search.grepBuffer('first line\nsecond match line\nthird line', 'match', {
    fixedStrings: true
});
// [{ line: 2, column: 8, text: 'second match line' }]

// ============================================================================
// 5. Linear-Time Regular Expressions (`Regex`, `regex`)
// ============================================================================

const re = new bro.search.Regex('\\b[A-Za-z_][A-Za-z0-9_]*\\b', {
    caseInsensitive: false,
    smartCase: false,
    multiline: true,
    dotMatchesNewline: false
});

console.log('Matches identifier:', re.test('my_var_1')); // or re.isMatch(...)
const first = re.find('   hello_world = 42;');           // { start: 3, end: 14, match: 'hello_world' }
const all = re.findAll('foo + bar_2');                   // [{ start, end, match }, ...]
