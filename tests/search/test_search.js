// Headless test for bro.search
assert(typeof bro.search === 'object', 'bro.search namespace exists');
if (!bro.search.available) {
    skipTest('bro.search is compiled out of this build');
} else {
    // 1. Fuzzy matching
    const candidates = [
        'src/bronze_host/host_sibling_apis.cpp',
        'src/render/vulkan_context.cpp',
        'include/broconf/api.h',
        'tests/test_app/index.html'
    ];

    const matches = bro.search.fuzzy('sibling', candidates);
    assert(Array.isArray(matches) && matches.length > 0, 'fuzzy returns match array');
    assert(matches[0].item === 'src/bronze_host/host_sibling_apis.cpp', 'top match is sibling_apis');
    assert(matches[0].score > 0, 'top match score is positive');
    assert(Array.isArray(matches[0].positions) && matches[0].positions.length > 0, 'positions returned');

    // 2. FuzzyIndex
    const index = new bro.search.FuzzyIndex(candidates);
    assert(typeof index === 'object', 'FuzzyIndex created');
    const indexMatches = index.search('vulkan');
    assert(Array.isArray(indexMatches) && indexMatches.length > 0, 'index.search returns matches');
    assert(indexMatches[0].item.includes('vulkan'), 'matched item contains vulkan');

    // 3. Regex
    const regex = new bro.search.Regex('^[a-z]+_[a-z]+');
    assert(regex.test('hello_world') === true, 'regex tests true');
    assert(regex.test('123_bad') === false, 'regex tests false');

    // 4. GitignoreMatcher
    const gm = new bro.search.GitignoreMatcher();
    gm.add('*.tmp');
    gm.add('build/');
    assert(gm.isIgnored('file.tmp') === true, '*.tmp is ignored');
    assert(gm.isIgnored('file.txt') === false, '*.txt is not ignored');

    // 5. Files walk & Grep
    const files = bro.search.filesSync('tests/test_app', { maxDepth: 2 });
    assert(Array.isArray(files) && files.length > 0, 'filesSync returns files');

    const hits = bro.search.grepSync('tests/test_app', 'html');
    assert(Array.isArray(hits), 'grepSync returns array of hits');

    console.log('test_search.js PASSED');
}
