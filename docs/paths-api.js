// =============================================================================
// bro.appDir / bro.resolvePath — an app naming its own files
// =============================================================================
//
// An app can already READ its own assets by virtual path: `/app/logo.png` in an
// <img src>, an import, or any binding that takes a file path. That covers most
// apps completely, and if it covers yours you do not need this file.
//
// What it does not cover is handing a path to something outside the engine:
//
//   - launching a binary the app ships next to itself (`bin/ffmpeg.exe`)
//   - passing a filename to a child process, which cannot see engine mounts
//   - printing a path for the user, or writing one into a config file
//
// For those you need a real filesystem path, and the obvious guesses are both
// wrong. `process.cwd()` is wherever the user launched bro from — usually not
// the app directory, and never guaranteed to be. `__dirname` exists only inside
// modules loaded through require(), not in app scripts or ES modules.
//
//   bro.appDir                  // absolute path of the app's own root
//   bro.resolvePath(path)       // any accepted spelling → absolute path
//
// Both use native separators (backslashes on Windows), because their whole
// purpose is to be handed to something outside the engine.


// -----------------------------------------------------------------------------
// bro.appDir
// -----------------------------------------------------------------------------
//
// The directory the running app was loaded from. Empty string when there isn't
// one (a bare `bro-headless -e "..."` session), so test it before joining onto
// it if your code can run that way.

const path = require('path');
const bundledFfmpeg = path.join(bro.appDir, 'bin', 'ffmpeg.exe');


// -----------------------------------------------------------------------------
// bro.resolvePath(path)
// -----------------------------------------------------------------------------
//
// Applies the same rules every path-taking binding uses, and returns the result
// instead of opening it. Four spellings go in; an absolute path comes out.
//
//   bro.resolvePath('bin/ffmpeg.exe')   // relative → against the APP dir,
//                                       //   not the cwd
//   bro.resolvePath('/app/preset.json') // mount path → real location
//   bro.resolvePath('/app')             // a bare mount root → the directory
//                                       //   itself (=== bro.appDir)
//   bro.resolvePath('C:/media/in.mov')  // already absolute → unchanged
//
// The last case is what makes it safe to pass user input straight in: you do
// not have to work out which shape you were given first.
//
// Resolution is textual, so a path that does not exist yet resolves fine. Use
// it for a file you are about to write, or to check whether an optional sidecar
// binary is actually present:

const fs = require('fs');
const ffmpeg = fs.existsSync(bundledFfmpeg) ? bundledFfmpeg : 'ffmpeg';   // else PATH


// -----------------------------------------------------------------------------
// Worked example: driving an external tool
// -----------------------------------------------------------------------------
//
// The pieces that make this work together: resolvePath turns the user's file
// and the app's own binary into real paths, and cp.execFile passes them as
// literal argv so a name like `mix & master (final).mov` survives intact.
// (See docs/brokit-api.js — exec takes a shell string, execFile does not.)

const cp = require('child_process');

async function probe(userFile) {
    const { stdout } = await cp.execFile(ffmpeg.replace('ffmpeg', 'ffprobe'), [
        '-v', 'quiet',
        '-print_format', 'json',
        '-show_streams',
        bro.resolvePath(userFile),
    ]);
    return JSON.parse(stdout);
}
