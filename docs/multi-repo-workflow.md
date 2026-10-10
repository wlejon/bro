# Working across bro and its sibling repos

[ecosystem.md](ecosystem.md) lists every repo; `scripts/repos.txt` is the same list for tooling. Each repo is a standalone checkout at `../<name>`. There are no submodules.

## How a dependency resolves

Every repo with dependencies carries the identical `cmake/bro_deps.cmake`. A `bro_dependency(<name> ...)` takes the first of:

1. a target that already exists (shared dependencies build once);
2. a working tree: `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>`, else `../<name>` (not for `THIRD_PARTY` deps);
3. a GitHub tarball into `build/_deps/<name>-src`, at the commit `cmake/bro_lock.cmake` names (release tags only), else the `REF` sha (third-party code), else the head of `main` read at this configure.

The configure prints which it took for each dependency (`bronze: working tree ...` / `bronze: github.com/wlejon/bronze <sha> (default branch)`). bro declares everything in `cmake/bro_pins.cmake`; its third-party pins win over any sibling's.

- Edit a sibling only in `../<name>`, never in `build/_deps/<name>-src`.
- Nothing to bump: a pushed sibling commit is what CI and fresh clones build next.
- Offline: `-DBRO_DEPS_OFFLINE=ON` (or a failed lookup) reuses the commits that build dir last resolved. A build dir that never resolved a dependency cannot.
- To build what CI builds, configure from a clone with no siblings beside it.
- `BRASS_ROOT` (cache or environment) points brass elsewhere.
- Keep a call's `name`, `GITHUB` and `REF` on one line; the scripts below match them line by line.
- `scripts/sync-deps.sh [--check]` copies bro's `bro_deps.cmake` to every repo and drops any `REF` on a wlejon dependency.

Which siblings a build adds follows the `BRO_WITH_*` flags ([build-options.md](build-options.md)).

## Day to day

```bash
# bro builds every sibling from ../<name>
cmake --build build --config Release

# a sibling's own tests
cd ../brokit   && cmake --build build --config Debug && ./build/tests/Debug/brokit_test.exe tests/js
cd ../bromesh  && cmake --build build --config Release && ./build/tests/Release/bromesh_test.exe   # Release only: meshoptimizer's Debug asserts open a modal dialog
cd ../brolm    && ctest --test-dir build -C Release -R brolm_test_api                              # the JS binding's test

# bronze
cd ../bronze && ./dev.cmd cmake --preset dev && ./dev.cmd cmake --build --preset dev && ./dev.cmd ctest --preset dev -LE "threejs|pixi"
```

Every library except bromath, htmlayout, bropty, bromux, brolink, brovideo, brodmabuf and brodbus owns its JS binding, `<name>_api`, under its `src/api/`. bro mounts them only in `installSiblingApis` (`src/bronze_host/host_sibling_apis.cpp`).

## Pushing

Push in dependency order (libraries, then bro, then apps) so no pushed main needs a commit GitHub lacks. `scripts/repo-status.sh --push` does that. A bronze embed change and the sibling bindings that follow it go out as one run of pushes, bronze first; CI caught in between fails once.

```bash
scripts/repo-status.sh              # every repo: branch, dirty, ahead/behind, dependency report
scripts/repo-status.sh --pull       # fast-forward-only pull of every repo, then report
scripts/repo-status.sh --push       # push repos ahead of upstream, in dependency order
pwsh scripts/repo-status.ps1 [-ListFiles] [-Pull] [-Push]   # the Windows port
```

`--pull` skips (and reports) diverged repos, detached HEADs and branches with no upstream.

## Releases: lock, tag, unlock

A tag carries `cmake/bro_lock.cmake`, one `bro_lock(<name> <sha>)` per ecosystem dependency. Main never does.

```bash
scripts/lock-deps.sh                 # lock every dependency at its GitHub main head
scripts/lock-deps.sh --local         # ...or at the ../<name> HEADs (warns if unpushed or dirty)
scripts/lock-deps.sh --dry-run
git add cmake/bro_lock.cmake && git commit -m "Lock dependencies for v0.2.0"
git tag v0.2.0 && git push origin main v0.2.0
scripts/lock-deps.sh --unlock
git commit -m "Unlock dependencies after v0.2.0" -- cmake/bro_lock.cmake && git push
```

`--repo <dir>` locks another repo (helm, ffmpeg-bro) the same way.

## Apps

[broworkshop](https://github.com/wlejon/broworkshop) at `../broworkshop` holds the launcher and apps (`games/`, `tools/`, `demos/`, `ai/`, shared `lib/`). It is not a build dependency.

```bash
bro ../broworkshop                   # the project's default_app (the launcher)
bro ../broworkshop/games/snake       # one app
```
