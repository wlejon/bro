// What this process costs, for `bro-ctl mem` (docs/agent-control.md): the
// operating system's view (private and shared memory, by kind of mapping),
// the threads by name, the C heap, the GPU memory the driver charges the
// process, and the engine's own accounting — which subsystems are running,
// and what the JS heap, Skia's caches and the image store hold.
//
// The point is attribution: a number that grows, or a subsystem an app never
// touched that is running anyway, shows up here by name. Reading smaps walks
// every mapping, so this is a command to run, not a per-frame call.

#include "bronze_host/bronze_host.h"
#include "engine/engine.h"
#include "render/image_store.h"
#include "render/skia_gpu.h"
#include "util/exe_dir.h"

#include <include/core/SkGraphics.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>
#elif defined(__linux__)
#  include <dirent.h>
#  include <fstream>
#  include <sstream>
#  if defined(__GLIBC__)
#    include <malloc.h>
#  endif
#elif defined(__APPLE__)
#  include <mach/mach.h>
#endif

namespace bro::engine {

namespace {

std::string mb(double bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

std::string mbKb(uint64_t kb) { return mb(static_cast<double>(kb) * 1024.0); }

#if defined(__linux__)

// glibc puts every arena but the main one in heaps aligned to HEAP_MAX_SIZE
// (64 MB on 64-bit), so an anonymous read-write mapping that starts on such a
// boundary is, in practice, a malloc arena.
constexpr uint64_t kArenaAlign = uint64_t{64} << 20;

struct MapEntry {
    std::string name;
    uint64_t start = 0, sizeKb = 0, rssKb = 0, privDirtyKb = 0, privCleanKb = 0, sharedCleanKb = 0,
             sharedDirtyKb = 0;
    std::string perms;
};

std::vector<MapEntry> readSmaps() {
    std::vector<MapEntry> out;
    std::ifstream f("/proc/self/smaps");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // A mapping's header starts with its hex range; the fields that follow
        // start with a capitalised name and a colon.
        const char c = line[0];
        const bool header = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) && line.find('-') < 17 &&
                            line.find(':') > line.find(' ');
        if (header) {
            MapEntry e;
            std::istringstream is(line);
            std::string range, offset, dev, inode;
            is >> range >> e.perms >> offset >> dev >> inode;
            std::getline(is, e.name);
            const size_t p = e.name.find_first_not_of(' ');
            e.name = p == std::string::npos ? std::string() : e.name.substr(p);
            e.start = std::strtoull(range.c_str(), nullptr, 16);
            out.push_back(std::move(e));
            continue;
        }
        if (out.empty()) continue;
        uint64_t v = 0;
        if (std::sscanf(line.c_str(), "Size: %" SCNu64, &v) == 1) out.back().sizeKb = v;
        else if (std::sscanf(line.c_str(), "Rss: %" SCNu64, &v) == 1) out.back().rssKb = v;
        else if (std::sscanf(line.c_str(), "Private_Dirty: %" SCNu64, &v) == 1) out.back().privDirtyKb = v;
        else if (std::sscanf(line.c_str(), "Private_Clean: %" SCNu64, &v) == 1) out.back().privCleanKb = v;
        else if (std::sscanf(line.c_str(), "Shared_Clean: %" SCNu64, &v) == 1) out.back().sharedCleanKb = v;
        else if (std::sscanf(line.c_str(), "Shared_Dirty: %" SCNu64, &v) == 1) out.back().sharedDirtyKb = v;
    }
    return out;
}

// The file holding the engine's code (libbro.so, or the executable when it is
// linked in statically): what of it is resident, and how much of that other
// processes share. Code pages of one file are shared clean across every
// process that maps it; relocated data (.data.rel.ro, the GOT) is private.
void appendEngineImage(std::string& out, const std::vector<MapEntry>& maps) {
    const std::string image = util::engineImagePath();
    uint64_t rss = 0, sharedClean = 0, privClean = 0, privDirty = 0, sharedDirty = 0, codeRss = 0, codeShared = 0;
    for (const auto& e : maps) {
        if (e.name != image) continue;
        rss += e.rssKb;
        sharedClean += e.sharedCleanKb;
        sharedDirty += e.sharedDirtyKb;
        privClean += e.privCleanKb;
        privDirty += e.privDirtyKb;
        if (e.perms.size() > 2 && e.perms[2] == 'x') {
            codeRss += e.rssKb;
            codeShared += e.sharedCleanKb;
        }
    }
    out += "engine image  " + image + "\n";
    out += "  resident " + mbKb(rss) + "  shared clean " + mbKb(sharedClean) + "  private clean " + mbKb(privClean) +
           "  private dirty " + mbKb(privDirty) + (sharedDirty ? "  shared dirty " + mbKb(sharedDirty) : "") + "\n";
    out += "  code: resident " + mbKb(codeRss) + ", shared with other processes " + mbKb(codeShared) + "\n";
}

std::map<std::string, uint64_t> readRollup() {
    std::map<std::string, uint64_t> out;
    std::ifstream f("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(f, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos || line.find("kB") == std::string::npos) continue;
        out[line.substr(0, colon)] = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
    }
    return out;
}

std::string classify(const MapEntry& e) {
    if (e.name == "[heap]") return "malloc main heap [heap]";
    if (e.name == "[stack]") return "main thread stack";
    if (e.name.rfind("/dev/dri/", 0) == 0) return "GPU buffer mappings";
    if (e.name.rfind("/memfd:", 0) == 0 || e.name.rfind("/dev/shm/", 0) == 0 ||
        e.name.rfind("/SYSV", 0) == 0)
        return "shared memory (memfd/shm)";
    if (e.name.empty()) {
        if (e.perms.rfind("rw", 0) == 0 && e.start % kArenaAlign == 0) return "malloc thread arenas";
        return "anonymous (JS heap, JIT code, thread stacks, drivers)";
    }
    if (e.name[0] == '[') return "kernel " + e.name;
    return "file-backed private (relocations, .data)";
}

void appendLinuxProcess(std::string& out, bool detail) {
    auto r = readRollup();
    out += "process   rss " + mbKb(r["Rss"]) + "  private " + mbKb(r["Private_Dirty"] + r["Private_Clean"]) +
           " (dirty " + mbKb(r["Private_Dirty"]) + ")  shared " + mbKb(r["Shared_Clean"] + r["Shared_Dirty"]) +
           "  pss " + mbKb(r["Pss"]) + "  swap " + mbKb(r["Swap"]) + "\n";

    const auto maps = readSmaps();
    appendEngineImage(out, maps);
    std::map<std::string, std::pair<uint64_t, uint64_t>> byClass;  // private dirty, count
    std::map<std::string, uint64_t> byFile;
    for (const auto& e : maps) {
        if (!e.privDirtyKb) continue;
        auto& c = byClass[classify(e)];
        c.first += e.privDirtyKb;
        ++c.second;
        if (!e.name.empty() && e.name[0] == '/') byFile[e.name] += e.privDirtyKb;
    }
    std::vector<std::pair<uint64_t, std::string>> rows;
    for (const auto& [k, v] : byClass) rows.push_back({v.first, k});
    std::sort(rows.rbegin(), rows.rend());
    out += "private dirty by mapping kind:\n";
    for (const auto& [kb, name] : rows) {
        char line[160];
        std::snprintf(line, sizeof line, "  %10s  %s (%" PRIu64 ")\n", mbKb(kb).c_str(), name.c_str(),
                      byClass[name].second);
        out += line;
    }
    if (detail) {
        std::vector<std::pair<uint64_t, std::string>> files;
        for (const auto& [k, v] : byFile) files.push_back({v, k});
        std::sort(files.rbegin(), files.rend());
        out += "private dirty by file (top 12):\n";
        for (size_t i = 0; i < files.size() && i < 12; ++i)
            out += "  " + mbKb(files[i].first) + "  " + files[i].second + "\n";
        std::vector<const MapEntry*> big;
        for (const auto& e : maps)
            if (e.name.empty() && e.privDirtyKb) big.push_back(&e);
        std::sort(big.begin(), big.end(), [](auto* a, auto* b) { return a->privDirtyKb > b->privDirtyKb; });
        out += "largest anonymous mappings (top 16):\n";
        for (size_t i = 0; i < big.size() && i < 16; ++i) {
            char line[160];
            std::snprintf(line, sizeof line, "  %10s of %10s  %s at %" PRIx64 "\n",
                          mbKb(big[i]->privDirtyKb).c_str(), mbKb(big[i]->sizeKb).c_str(), big[i]->perms.c_str(),
                          big[i]->start);
            out += line;
        }
    }
}

void appendLinuxThreads(std::string& out) {
    std::map<std::string, int> names;
    int total = 0;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* de = readdir(d)) {
            if (de->d_name[0] == '.') continue;
            std::ifstream f(std::string("/proc/self/task/") + de->d_name + "/comm");
            std::string name;
            std::getline(f, name);
            // "Worker 12", "bro:disk$3": one row per family.
            while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back())) || name.back() == ' ' ||
                                     name.back() == '$' || name.back() == '.'))
                name.pop_back();
            ++names[name.empty() ? "?" : name];
            ++total;
        }
        closedir(d);
    }
    std::vector<std::pair<int, std::string>> rows;
    for (const auto& [k, v] : names) rows.push_back({v, k});
    std::sort(rows.rbegin(), rows.rend());
    out += "threads   " + std::to_string(total) + ":";
    for (const auto& [n, name] : rows) out += " " + name + (n > 1 ? " x" + std::to_string(n) : "") + ",";
    if (out.back() == ',') out.pop_back();
    out += "\n";
}

void appendLinuxMalloc(std::string& out) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 mi = mallinfo2();
    int arenas = 0;
    char* buf = nullptr;
    size_t len = 0;
    if (FILE* mf = open_memstream(&buf, &len)) {
        malloc_info(0, mf);
        std::fclose(mf);
        for (const char* p = buf; p && (p = std::strstr(p, "<heap nr=")); ++p) ++arenas;
        std::free(buf);
    }
    out += "malloc    in use " + mb(static_cast<double>(mi.uordblks + mi.hblkhd)) + "  free in arenas " +
           mb(static_cast<double>(mi.fordblks)) + "  main heap " + mb(static_cast<double>(mi.arena)) +
           "  mmapped " + mb(static_cast<double>(mi.hblkhd)) + "  arenas " + std::to_string(arenas) + "\n";
#else
    (void)out;
#endif
}

void appendLinuxGpu(std::string& out) {
    // amdgpu/i915/xe/nouveau report per DRM client in fdinfo; one client can
    // be open on several fds, so count each client id once.
    std::set<std::string> seen;
    uint64_t vramKb = 0, gttKb = 0;
    int clients = 0;
    DIR* d = opendir("/proc/self/fdinfo");
    if (!d) return;
    while (dirent* de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        std::ifstream f(std::string("/proc/self/fdinfo/") + de->d_name);
        std::string line, client;
        uint64_t vram = 0, gtt = 0;
        bool drm = false;
        while (std::getline(f, line)) {
            uint64_t v = 0;
            char key[64] = {};
            if (line.rfind("drm-client-id:", 0) == 0) {
                drm = true;
                client = line.substr(14);
            } else if (std::sscanf(line.c_str(), "%63[^:]: %" SCNu64, key, &v) == 2) {
                const std::string k = key;
                if (k == "drm-memory-vram" || k == "drm-total-vram" || k == "drm-total-local0") vram = std::max(vram, v);
                if (k == "drm-memory-gtt" || k == "drm-total-gtt" || k == "drm-total-system0") gtt = std::max(gtt, v);
            }
        }
        if (!drm || !seen.insert(client).second) continue;
        ++clients;
        vramKb += vram;
        gttKb += gtt;
    }
    closedir(d);
    if (clients)
        out += "gpu       vram " + mbKb(vramKb) + "  gtt " + mbKb(gttKb) + "  (" + std::to_string(clients) +
               " DRM client" + (clients > 1 ? "s" : "") + ", fdinfo)\n";
}

#elif defined(_WIN32)

void appendWindowsProcess(std::string& out) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        out += "process   working set " + mb(static_cast<double>(pmc.WorkingSetSize)) + "  private " +
               mb(static_cast<double>(pmc.PrivateUsage)) + "  peak working set " +
               mb(static_cast<double>(pmc.PeakWorkingSetSize)) + "\n";
    int threads = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) ++threads;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    out += "threads   " + std::to_string(threads) + "\n";
}

#elif defined(__APPLE__)

void appendAppleProcess(std::string& out) {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        out += "process   rss " + mb(static_cast<double>(info.resident_size)) + "  virtual " +
               mb(static_cast<double>(info.virtual_size)) + "  peak rss " +
               mb(static_cast<double>(info.resident_size_max)) + "\n";
    }
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t nthreads = 0;
    if (task_threads(mach_task_self(), &list, &nthreads) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nthreads; ++i) mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), nthreads * sizeof(thread_act_t));
    }
    out += "threads   " + std::to_string(nthreads) + "\n";
}

#endif

}  // namespace

std::string Engine::memoryReport(bool detail) {
    std::string out;
#if defined(__linux__)
    appendLinuxProcess(out, detail);
    appendLinuxThreads(out);
    appendLinuxMalloc(out);
    appendLinuxGpu(out);
#elif defined(_WIN32)
    (void)detail;
    appendWindowsProcess(out);
#elif defined(__APPLE__)
    (void)detail;
    appendAppleProcess(out);
#else
    (void)detail;
#endif

    const bronze_host::JsMemoryStats js = bronze_host::jsMemoryStats();
    out += "js        heap used " + mb(static_cast<double>(js.heapUsed)) + "  committed " +
           mb(static_cast<double>(js.heapCommitted)) + "  reserved " + mb(static_cast<double>(js.heapReserved)) +
           "  collections " + std::to_string(js.collections) + "\n";
    const bronze_host::CompiledBodyStats bodies = bronze_host::compiledBodyStats();
    out += "mir       function bodies held " + mb(static_cast<double>(bodies.liveBytes)) + "  built " +
           std::to_string(bodies.built) + "  released " + std::to_string(bodies.released) + "\n";

    out += "skia      glyph cache " + mb(static_cast<double>(SkGraphics::GetFontCacheUsed())) + " of " +
           mb(static_cast<double>(SkGraphics::GetFontCacheLimit())) + "  cpu resource cache " +
           mb(static_cast<double>(SkGraphics::GetResourceCacheTotalBytesUsed())) + "\n";
    const render::SkiaGpu::MemoryStats g = render::SkiaGpu::targetMemoryStats();
    if (g.valid)
        out += "ganesh    resources " + mb(static_cast<double>(g.ganeshBytes)) + " (purgeable " +
               mb(static_cast<double>(g.ganeshPurgeableBytes)) + ") of " + mb(static_cast<double>(g.ganeshLimit)) +
               "  device pool host-visible " + mb(static_cast<double>(g.poolHostVisibleBytes)) + "  device-only " +
               mb(static_cast<double>(g.poolDeviceOnlyBytes)) + "\n";
    {
        render::ImageStore& store = render::ImageStore::instance();
        out += "images    " + std::to_string(store.cachedCount()) + " decoded, " +
               mb(static_cast<double>(store.cachedBytes())) + " of " + mb(static_cast<double>(store.budgetBytes())) +
               "  decoder threads " + std::to_string(store.decoderThreads()) + "\n";
    }

    // The subsystems an app pays for only once it uses them.
    auto on = [](bool b) { return b ? "on" : "off"; };
    out += "subsystems";
#if BRO_WITH_PHYSICS
    out += std::string(" physics:") + on(physicsWorld_ != nullptr);
#endif
    out += std::string(" audio:") + on(audioEngine_ != nullptr);
    out += std::string(" audio-inference:") + on(audioInference_ != nullptr);
#if BRO_WITH_NET
    out += std::string(" net:") + on(netService_ != nullptr);
#endif
    out += std::string(" steam:") + on(steamService_ != nullptr);
#if BRO_WITH_3D
    out += " scenes:" + std::to_string(sceneGraphs_.size());
#endif
    out += "\n";
    // The wrapper modules entered when a program first touches them.
    const std::string loaded = bronze_host::loadedModuleGroups();
    const std::string deferred = bronze_host::deferredModuleGroups();
    out += "modules   loaded: " + (loaded.empty() ? std::string("-") : loaded) +
           "  deferred: " + (deferred.empty() ? std::string("-") : deferred) + "\n";
    return out;
}

void Engine::releaseFreeHeapPages() {
#if defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
#endif
}

void Engine::configureHeap() {
#if defined(__linux__) && defined(__GLIBC__)
    static bool done = false;
    if (done) return;
    done = true;
    int arenas = 4;
    if (const char* env = std::getenv("BRO_MALLOC_ARENAS")) arenas = std::atoi(env);
    if (arenas > 0) mallopt(M_ARENA_MAX, arenas);
#endif
}

std::string Engine::trimMemory() {
    std::string js = "js heap released " + mb(static_cast<double>(bronze_host::releaseIdleJsHeap())) +
                     " of young pages\n";
    js += "script function bodies released " +
          mb(static_cast<double>(bronze_host::releaseIdleCompiledBodies())) + "\n";
#if defined(__linux__) && defined(__GLIBC__)
    auto before = readRollup()["Private_Dirty"];
    malloc_trim(0);
    auto after = readRollup()["Private_Dirty"];
    return js + "malloc_trim released " + mbKb(before > after ? before - after : 0) + "\n";
#elif defined(_WIN32)
    HeapCompact(GetProcessHeap(), 0);
    return js + "HeapCompact on the process heap\n";
#else
    return js;
#endif
}

}  // namespace bro::engine
