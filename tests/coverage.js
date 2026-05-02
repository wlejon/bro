// Test coverage analyzer for bro.
// Runs via: bro-headless --no-gpu ../broworkshop/demos/example tests/coverage.js
//
// Scans src/js/*_bindings.cpp for the JS API surface (.method/.function/.getter/.property),
// then scans all test files (tests/**\/test_*.js + ../broworkshop/**\/test*.js) for usage.
// Outputs a per-binding-file coverage report.

var fs = globalThis.__brokit_fs;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

var SRC_DIR   = 'src/js';
var TEST_DIRS = ['tests', '../broworkshop'];

// Patterns that register JS-visible names in the C++ bindings
// Matches: .method("name"   .function("name"   .getter("name"   .property("name"
var BINDING_RE = /\.(method|function|getter|setter|property)\("([^"]+)"/;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function readText(path) {
    return fs.readFileSync(path, 'utf8');
}

function listFilesRecursive(dir, filter) {
    var results = [];
    var entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); }
    catch (e) { return results; }
    for (var i = 0; i < entries.length; i++) {
        var entry = entries[i];
        var full = dir + '/' + entry.name;
        if (entry.isDirectory()) {
            // skip node_modules, build, third_party, .git, output dirs
            if (entry.name === 'node_modules' || entry.name === 'build' ||
                entry.name === 'third_party'  || entry.name === '.git' ||
                entry.name === 'output')
                continue;
            results = results.concat(listFilesRecursive(full, filter));
        } else if (filter(entry.name)) {
            results.push(full);
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// 1. Extract JS API surface from binding files
// ---------------------------------------------------------------------------

var bindingFiles = listFilesRecursive(SRC_DIR, function(name) {
    return name.endsWith('_bindings.cpp') || name === 'custom_elements.cpp';
});

// Map: binding file -> array of { name, kind, line }
var apiSurface = Object.create(null);
var allApiNames = Object.create(null);  // name -> [{ file, kind }]

for (var i = 0; i < bindingFiles.length; i++) {
    var bfile = bindingFiles[i];
    var lines = readText(bfile).split('\n');
    var entries = [];

    for (var ln = 0; ln < lines.length; ln++) {
        var match = lines[ln].match(BINDING_RE);
        if (match) {
            var kind = match[1];
            var name = match[2];
            entries.push({ name: name, kind: kind, line: ln + 1 });
            if (!allApiNames[name]) allApiNames[name] = [];
            allApiNames[name].push({ file: bfile, kind: kind });
        }
    }

    // Also pick up JS_SetPropertyStr for direct property installs
    // Pattern: JS_SetPropertyStr(ctx, obj, "name", ...)
    var setPropRe = /JS_SetPropertyStr\([^,]+,\s*[^,]+,\s*"([a-zA-Z_]\w*)"/;
    for (var ln = 0; ln < lines.length; ln++) {
        var match = lines[ln].match(setPropRe);
        if (match) {
            var name = match[1];
            // Skip internal/duplicate names and constructor/prototype wiring
            if (name === 'constructor' || name === 'prototype' || name === 'length')
                continue;
            // Skip if already captured by method/function/etc
            var dup = false;
            for (var e = 0; e < entries.length; e++) {
                if (entries[e].name === name) { dup = true; break; }
            }
            if (!dup) {
                entries.push({ name: name, kind: 'property', line: ln + 1 });
                if (!allApiNames[name]) allApiNames[name] = [];
                allApiNames[name].push({ file: bfile, kind: 'property' });
            }
        }
    }

    if (entries.length > 0)
        apiSurface[bfile] = entries;
}

// ---------------------------------------------------------------------------
// 2. Collect all test file content
// ---------------------------------------------------------------------------

var testFiles = [];
for (var d = 0; d < TEST_DIRS.length; d++) {
    testFiles = testFiles.concat(
        listFilesRecursive(TEST_DIRS[d], function(name) {
            return name.match(/^test.*\.js$/) !== null;
        })
    );
}

// Build a combined token set from all test files
var testTokens = Object.create(null);
var testContent = '';

for (var i = 0; i < testFiles.length; i++) {
    var content = readText(testFiles[i]);
    testContent += '\n' + content;

    // Extract identifiers (rough but effective)
    var tokens = content.match(/[a-zA-Z_$][a-zA-Z0-9_$]*/g);
    if (tokens) {
        for (var t = 0; t < tokens.length; t++) {
            var tok = tokens[t];
            if (!testTokens[tok]) testTokens[tok] = [];
            testTokens[tok].push(testFiles[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Report
// ---------------------------------------------------------------------------

console.log('═══════════════════════════════════════════════════════════════');
console.log('  BRO TEST COVERAGE — JS API SURFACE ANALYSIS');
console.log('═══════════════════════════════════════════════════════════════');
console.log('');
console.log('Binding files scanned: ' + Object.keys(apiSurface).length);
console.log('Test files scanned:    ' + testFiles.length);
console.log('');

var totalApi = 0;
var totalCovered = 0;
var totalUncovered = 0;
var uncoveredByFile = Object.create(null);

var fileNames = Object.keys(apiSurface).sort();

for (var fi = 0; fi < fileNames.length; fi++) {
    var fname = fileNames[fi];
    var entries = apiSurface[fname];
    var covered = [];
    var uncovered = [];

    for (var j = 0; j < entries.length; j++) {
        var api = entries[j];
        // Check if this name appears in any test file
        if (testTokens[api.name]) {
            covered.push(api);
        } else {
            uncovered.push(api);
        }
    }

    totalApi += entries.length;
    totalCovered += covered.length;
    totalUncovered += uncovered.length;

    var pct = entries.length > 0
        ? Math.round(covered.length / entries.length * 100)
        : 100;

    // Short filename for display
    var short = fname.replace(/^src\/js\//, '');

    var bar = '';
    var barLen = 20;
    var filled = Math.round(pct / 100 * barLen);
    for (var b = 0; b < barLen; b++)
        bar += (b < filled) ? '#' : '.';

    console.log('── ' + short + ' ──');
    console.log('   [' + bar + '] ' + pct + '%  (' + covered.length + '/' + entries.length + ')');

    if (uncovered.length > 0) {
        uncoveredByFile[short] = uncovered;
        var names = [];
        for (var u = 0; u < uncovered.length; u++)
            names.push(uncovered[u].name);
        console.log('   Missing: ' + names.join(', '));
    }
    console.log('');
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

var overallPct = totalApi > 0 ? Math.round(totalCovered / totalApi * 100) : 100;

console.log('═══════════════════════════════════════════════════════════════');
console.log('  SUMMARY');
console.log('═══════════════════════════════════════════════════════════════');
console.log('');
console.log('  Total API functions/properties: ' + totalApi);
console.log('  Covered by tests:               ' + totalCovered + ' (' + overallPct + '%)');
console.log('  Not covered:                    ' + totalUncovered);
console.log('');

// Group uncovered by category
var categories = {
    'DOM':     ['element_bindings', 'node_bindings', 'document_bindings',
                'dom_bindings', 'shadowroot_bindings'],
    'Events':  ['event_bindings', 'event_dispatch'],
    'Style':   ['style_bindings'],
    'Canvas':  ['canvas_bindings'],
    'WebGL':   ['webgl2_bindings'],
    'Scene':   ['scene_bindings', 'terrain_bindings', 'mesh_bindings'],
    'Audio':   ['audio_bindings'],
    'Physics': ['physics_bindings'],
    'Input':   ['headless_bindings'],
    'Other':   ['custom_elements', 'dialog_bindings', 'image_bindings',
                'settings_bindings', 'storage_bindings', 'timers',
                'window_bindings', 'worker']
};

console.log('  Uncovered by category:');
var catNames = Object.keys(categories);
for (var ci = 0; ci < catNames.length; ci++) {
    var cat = catNames[ci];
    var prefixes = categories[cat];
    var catUncovered = 0;
    var catTotal = 0;
    for (var p = 0; p < prefixes.length; p++) {
        for (var fi = 0; fi < fileNames.length; fi++) {
            if (fileNames[fi].indexOf(prefixes[p]) !== -1) {
                var entries = apiSurface[fileNames[fi]];
                catTotal += entries.length;
                for (var j = 0; j < entries.length; j++) {
                    if (!testTokens[entries[j].name]) catUncovered++;
                }
            }
        }
    }
    if (catTotal > 0) {
        var catPct = Math.round((catTotal - catUncovered) / catTotal * 100);
        var label = cat + ':';
        while (label.length < 12) label += ' ';
        console.log('    ' + label + (catTotal - catUncovered) + '/' + catTotal + ' (' + catPct + '%)');
    }
}

console.log('');
console.log('  Modules with NO test coverage at all:');

// Check for source modules with zero tests
var srcModules = ['canvas', 'render', 'webgl', 'scene', 'physics', 'svg', 'util', 'platform'];
for (var m = 0; m < srcModules.length; m++) {
    var mod = srcModules[m];
    // Check if any test file references anything from this module
    var hasBindings = false;
    for (var bi = 0; bi < fileNames.length; bi++) {
        if (fileNames[bi].indexOf(mod) !== -1) { hasBindings = true; break; }
    }
    if (!hasBindings) {
        // No bindings file — purely internal C++ module
        console.log('    - ' + mod + '/ (no JS bindings — C++ internal only, untestable via headless JS)');
    }
}

console.log('');
console.log('  Note: This analysis checks if a JS API name appears as a token');
console.log('  in any test file. False positives are possible if a name matches');
console.log('  coincidentally. False negatives should be rare.');
console.log('');

// List test files for reference
console.log('── Test files (' + testFiles.length + ') ──');
for (var i = 0; i < testFiles.length; i++)
    console.log('  ' + testFiles[i]);
