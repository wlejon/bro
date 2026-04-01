# Dual-Repo Workflow: bro + htmlayout

bro depends on htmlayout as a git submodule at `third_party/htmlayout`. The standalone htmlayout repo lives at `../htmlayout`.

## Directory Layout

```
D:/projects/
├── bro/                        # main project
│   └── third_party/htmlayout/  # submodule (used by CI / fallback)
└── htmlayout/                  # standalone repo (preferred for dev)
```

## How It Works

bro's CMake auto-detects the standalone repo at `../htmlayout`. If found, it builds from there directly — **no submodule copy involved**. This means:

- **Edit once** — only touch files in `D:/projects/htmlayout`
- **One build** — `cmake --build build` in bro compiles the standalone htmlayout source
- The submodule is only used when the standalone repo isn't present (CI, fresh clones)

The detection is in `third_party/CMakeLists.txt`:
```cmake
set(HTMLAYOUT_DIR "${CMAKE_SOURCE_DIR}/../htmlayout" CACHE PATH "...")
if(EXISTS "${HTMLAYOUT_DIR}/CMakeLists.txt")
    add_subdirectory("${HTMLAYOUT_DIR}" "${CMAKE_BINARY_DIR}/htmlayout" EXCLUDE_FROM_ALL)
else()
    add_subdirectory(htmlayout EXCLUDE_FROM_ALL)
endif()
```

## Day-to-Day Development

### 1. Edit htmlayout

Edit files only in `D:/projects/htmlayout/src/...`.

### 2. Build and test

```bash
# Build bro (uses standalone htmlayout automatically)
cd D:/projects/bro
cmake --build build --config Debug

# Run htmlayout tests
cd D:/projects/htmlayout
cmake --build build --config Debug
./build/tests/Debug/htmlayout_test.exe

# Test bro
cd D:/projects/bro
echo -e "dump\nquit" | ./build/src/headless/Debug/bro-headless.exe apps/hello 2>/dev/null
```

### 3. Commit htmlayout

```bash
cd D:/projects/htmlayout
git add src/layout/foo.cpp
git commit -m "Fix description"
```

### 4. Sync submodule and commit bro

After htmlayout is committed, update the submodule pointer so CI and fresh clones get the right version:

```bash
cd D:/projects/bro/third_party/htmlayout
git fetch ../../../htmlayout main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/htmlayout
git commit -m "Update htmlayout: fix description"
```

## Overriding the Path

To use a different htmlayout location:

```bash
cmake -B build -DHTMLAYOUT_DIR=/path/to/htmlayout
```

Set to an empty/nonexistent path to force using the submodule:

```bash
cmake -B build -DHTMLAYOUT_DIR=none
```
