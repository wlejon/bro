# Multi-Repo Workflow: bro + sibling libraries

bro depends on six sibling libraries. Each has a standalone repo at `../<name>`. Three of them (the oldest) also have a git submodule fallback; the newer libraries require the standalone repo to be present.

| Library | Standalone repo | Submodule fallback |
|---------|----------------|-------------------|
| **qjsbind** | `../qjsbind` | — (standalone required) |
| **brokit** | `../brokit` | `third_party/brokit` |
| **htmlayout** | `../htmlayout` | `third_party/htmlayout` |
| **broaudio** | `../broaudio` | `third_party/broaudio` |
| **bromesh** | `../bromesh` | — (standalone required) |
| **brogameagent** | `../brogameagent` | — (standalone required) |

## Directory Layout

```
D:/projects/
├── bro/                          # main project
│   └── third_party/
│       ├── brokit/               # submodule (CI / fallback)
│       ├── htmlayout/            # submodule (CI / fallback)
│       └── broaudio/             # submodule (CI / fallback)
├── qjsbind/                      # standalone repo (required)
├── brokit/                       # standalone repo (preferred for dev)
├── htmlayout/                    # standalone repo (preferred for dev)
├── broaudio/                     # standalone repo (preferred for dev)
├── bromesh/                      # standalone repo (required)
└── brogameagent/                 # standalone repo (required)
```

## How It Works

bro's CMake auto-detects standalone repos at `../<name>`. If found, it builds from there directly — **no submodule copy involved**. This means:

- **Edit once** — only touch files in the standalone repo
- **One build** — `cmake --build build` in bro compiles every sibling from its standalone source
- Submodules are only used when standalone repos aren't present (CI, fresh clones) — and only for brokit, htmlayout, and broaudio. bromesh, brogameagent, and qjsbind have no submodule copy in `third_party/`, so a standalone clone at `../<name>` is mandatory.

The detection pattern in `third_party/CMakeLists.txt`:
```cmake
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "...")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
else()
    add_subdirectory(brokit EXCLUDE_FROM_ALL)
endif()
```

The same pattern is used for htmlayout, broaudio, bromesh, brogameagent, and qjsbind. For the three libraries without a submodule fallback, the `else` branch would fail to configure if the standalone repo is missing.

## Day-to-Day Development

### 1. Edit a library

Edit files only in the standalone repo (e.g. `D:/projects/brokit/src/...`, `D:/projects/bromesh/src/...`).

### 2. Build and test

```bash
# Build bro (uses standalone repos automatically)
cd D:/projects/bro
cmake --build build --config Debug

# Each sibling has its own build + tests; examples:
cd D:/projects/brokit && cmake --build build --config Debug
./build/tests/Debug/brokit_test.exe tests/js

cd D:/projects/htmlayout && cmake --build build --config Debug
./build/tests/Debug/htmlayout_test.exe

cd D:/projects/broaudio && cmake --build build --config Debug
./build/tests/Debug/broaudio_test.exe

# Note: bromesh tests must be built in Release — meshoptimizer's Debug
# assertions trigger a modal abort() dialog on Windows.
cd D:/projects/bromesh && cmake --build build --config Release
./build/tests/Release/bromesh_test.exe
```

### 3. Commit the library

```bash
cd D:/projects/brokit
git add src/api/new_api.cpp
git commit -m "Add new API"
```

### 4. Sync submodule and commit bro (only for brokit / htmlayout / broaudio)

For the three libraries with a submodule fallback, update the submodule pointer so CI and fresh clones pick up the change:

```bash
cd D:/projects/bro/third_party/brokit
git fetch ../../../brokit main
git checkout FETCH_HEAD

cd D:/projects/bro
git add third_party/brokit
git commit -m "Update brokit: add new API"
```

Same shape for `htmlayout` and `broaudio`.

For **bromesh**, **brogameagent**, and **qjsbind** there is no submodule to bump — downstream consumers are expected to clone the standalone repo directly.

## Overriding Paths

To point at a different location for any sibling:

```bash
cmake -B build \
    -DQJSBIND_DIR=/path/to/qjsbind \
    -DBROKIT_DIR=/path/to/brokit \
    -DHTMLAYOUT_DIR=/path/to/htmlayout \
    -DBROAUDIO_DIR=/path/to/broaudio \
    -DBROMESH_DIR=/path/to/bromesh \
    -DBROGAMEAGENT_DIR=/path/to/brogameagent
```

For the three libraries with a submodule, setting the `*_DIR` to a nonexistent path forces the submodule:

```bash
cmake -B build -DBROKIT_DIR=none -DHTMLAYOUT_DIR=none -DBROAUDIO_DIR=none
```

This trick does **not** work for bromesh, brogameagent, or qjsbind — the configure step will fail if their standalone repo is missing.
