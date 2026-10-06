// Headless test for bro.vfs
assert(typeof bro.vfs === 'object', 'bro.vfs namespace exists');
assert(bro.vfs.available === true, 'bro.vfs.available is true');

// 1. MIME and Volumes
const mime = bro.vfs.getMime('README.md');
assert(typeof mime === 'string' && mime.length > 0, 'getMime returns non-empty string');

const volumes = bro.vfs.listVolumes();
assert(Array.isArray(volumes), 'listVolumes returns an array');
if (volumes.length > 0) {
    assert(typeof volumes[0].mountPoint === 'string', 'volume has mountPoint');
    assert(typeof volumes[0].totalBytes === 'number', 'volume has totalBytes');
}

// 2. Directory scanning
await (async function() {
    const entries = await bro.vfs.scan('tests', { recursive: false });
    assert(Array.isArray(entries), 'scan returns an array');
    assert(entries.length > 0, 'tests directory has entries');
    const hasApps = entries.some(e => e.name === 'apps');
    assert(hasApps, 'scan found tests/apps');

    // 3. DirectoryModel
    const model = new bro.vfs.DirectoryModel('tests');
    assert(typeof model === 'object', 'DirectoryModel created');
    model.setSort('name', true);
    model.setFilter('*.js');
    const modelEntries = model.entries();
    assert(Array.isArray(modelEntries), 'model.entries returns array');

    // 4. File operations (copy, move, remove)
    const testDir = '/tmp/bro_vfs_test_headless_' + Date.now();
    const fs = require('fs');
    fs.mkdirSync(testDir, { recursive: true });
    const srcFile = testDir + '/source.txt';
    const dstFile = testDir + '/copy.txt';
    const mvFile = testDir + '/moved.txt';
    fs.writeFileSync(srcFile, 'Hello bro.vfs!');

    const copyRes = await bro.vfs.copy(srcFile, dstFile);
    assert(copyRes.ok === true, 'copy succeeded');
    assert(fs.existsSync(dstFile), 'dstFile exists');

    const moveRes = await bro.vfs.move(dstFile, mvFile);
    assert(moveRes.ok === true, 'move succeeded');
    assert(fs.existsSync(mvFile), 'mvFile exists');
    assert(!fs.existsSync(dstFile), 'dstFile no longer exists');

    // Undo / Redo
    if (bro.vfs.canUndo()) {
        const undone = await bro.vfs.undo();
        assert(typeof undone === 'boolean', 'undo returns boolean');
    }

    // Clean up testDir
    await bro.vfs.remove(testDir, { recursive: true });
    assert(!fs.existsSync(testDir), 'testDir removed');
})();

console.log('test_vfs.js PASSED');
