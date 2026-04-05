# Multi-Repo Workflow: bro + htmlayout + brokit + broaudio

bro depends on three sibling libraries, each with a standalone repo and a git submodule fallback:

| Library | Standalone repo | Submodule fallback |
|---------|----------------|-------------------|
| **htmlayout** | `../htmlayout` | `third_party/htmlayout` |
| **brokit** | `../brokit` | `third_party/brokit` |
| **broaudio** | `../broaudio` | `third_party/broaudio` |

## Directory Layout

```
D:/projects/
├── bro/                          # main project
│   └── third_party/
│       ├── htmlayout/            # submodule (CI / fallback)
│       ├── brokit/               # submodule (CI / fallback)
│       └── broaudio/             # submodule (CI / fallback)
├── htmlayout/                    # standalone repo (preferred for dev)
├── brokit/                       # standalone repo (preferred for dev)
└── broaudio/                     # standalone repo (preferred for dev)
```

## How It Works

bro's CMake auto-detects standalone repos at `../htmlayout` and `../brokit`. If found, it builds from there directly — **no submodule copy involved**. This means:

- **Edit once** — only touch files in the standalone repo
- **One build** — `cmake --build build` in bro compiles both libraries from their standalone source
- Submodules are only used when standalone repos aren't present (CI, fresh clones)

The detection is in `third_party/CMakeLists.txt`:
```cmake
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "...")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
else()
    add_subdirectory(brokit EXCLUDE_FROM_ALL)
endif()
```

The same pattern is used for htmlayout and broaudio.

## Day-to-Day Development

### 1. Edit a library

Edit files only in the standalone repo (e.g. `D:/projects/brokit/src/...`, `D:/projects/htmlayout/src/...`, or `D:/projects/broaudio/src/...`).

### 2. Build and test

```bash
# Build bro (uses standalone repos automatically)
cd D:/projects/bro
cmake --build build --config Debug

# Run brokit tests
cd D:/projects/brokit
cmake --build build --config Debug
./build/tests/Debug/brokit_test.exe tests/js

# Run htmlayout tests
cd D:/projects/htmlayout
cmake --build build --config Debug
./build/tests/Debug/htmlayout_test.exe

# Run broaudio tests
cd D:/projects/broaudio
cmake --build build --config Debug
./build/tests/Debug/broaudio_test.exe
```

### 3. Commit the library

```bash
cd D:/projects/brokit
git add src/api/new_api.cpp
git commit -m "Add new API"
```

### 4. Sync submodule and commit bro

After the library is committed, update the submodule pointer so CI and fresh clones get the right version:

```bash
cd D:/projects/bro/third_party/brokit
git fetch ../../../brokit main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/brokit
git commit -m "Update brokit: add new API"
```

Same for htmlayout:
```bash
cd D:/projects/bro/third_party/htmlayout
git fetch ../../../htmlayout main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/htmlayout
git commit -m "Update htmlayout: fix description"
```

Same for broaudio:
```bash
cd D:/projects/bro/third_party/broaudio
git fetch ../../../broaudio main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/broaudio
git commit -m "Update broaudio: description"
```

## Overriding Paths

To use a different location for either library:

```bash
cmake -B build -DBROKIT_DIR=/path/to/brokit -DHTMLAYOUT_DIR=/path/to/htmlayout -DBROAUDIO_DIR=/path/to/broaudio
```

Set to a nonexistent path to force using the submodule:

```bash
cmake -B build -DBROKIT_DIR=none -DBROAUDIO_DIR=none
```
