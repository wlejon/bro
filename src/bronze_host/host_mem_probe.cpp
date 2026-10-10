// __host.memory([sizes]): where the process's committed memory is, by who
// holds it. A leak hunt starts by asking which of these grows — the C/C++
// heaps (and, within them, which block sizes), the JavaScript heap, Skia's
// caches, or memory committed outside every heap (GPU drivers, the JS heap's
// own reservation, anything that calls VirtualAlloc itself) — rather than by
// guessing at a cache.
//
// Walking the heaps locks each one for the length of its walk and visits every
// block, so this is a diagnostic for probes, never a per-frame call.
//
// heapBusyBytes counts twice what the low-fragmentation heap holds: HeapWalk
// reports each LFH container (a 64 KB-ish busy block) and every block inside
// it. It is a number to compare with itself across a run, not with
// heapCommitted.

#include "bronze_host/host_runtime.h"
#include "bronze_host/host_builder.h"
#include "embed/embed.h"
#include "render/image_store.h"
#include "render/skia_gpu.h"

#include <include/core/SkGraphics.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace bro::bronze_host {

namespace {

struct HeapTotals {
    uint64_t committed = 0;
    uint64_t busyBytes = 0;
    uint64_t busyBlocks = 0;
    uint64_t freeBytes = 0;
    uint64_t freeBlocks = 0;
    uint64_t largestFree = 0;
};

Value num(uint64_t v) { return ev::fromDouble(static_cast<double>(v)); }

// A block this large lives in a VirtualAlloc of its own outside the heap's
// regions, so it is committed memory the region walk does not count.
constexpr uint64_t kLargeBlock = 508u * 1024u;

}  // namespace

Value hostMemoryBreakdown(bool sizes) {
    ObjectBuilder out;
#if defined(_WIN32)
    uint64_t privateCommitted = 0, imageCommitted = 0, mappedCommitted = 0, executableCommitted = 0;
    uint64_t privateRegions = 0;
    std::map<uint64_t, uint64_t> regionKinds;
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* p = nullptr;
        while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT) {
                if (mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                                               PAGE_EXECUTE_READWRITE)))
                    executableCommitted += mbi.RegionSize;
                if (mbi.Type == MEM_PRIVATE) {
                    privateCommitted += mbi.RegionSize;
                    ++privateRegions;
                    if (sizes) {
                        auto& r = regionKinds[(static_cast<uint64_t>(mbi.Protect) << 40) | mbi.RegionSize];
                        ++r;
                    }
                }
                else if (mbi.Type == MEM_IMAGE) imageCommitted += mbi.RegionSize;
                else if (mbi.Type == MEM_MAPPED) mappedCommitted += mbi.RegionSize;
            }
            p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (!p) break;
        }
    }

    std::vector<HANDLE> heaps(256);
    DWORD n = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    if (n > heaps.size()) {
        heaps.resize(n);
        n = GetProcessHeaps(n, heaps.data());
    }
    heaps.resize(std::min<size_t>(n, heaps.size()));

    std::map<uint64_t, std::pair<uint64_t, uint64_t>> bySize;
    HeapTotals all;
    ev::Persistent list(ev::makeArray(0));
    uint32_t idx = 0;
    for (HANDLE h : heaps) {
        HeapTotals t;
        if (!HeapLock(h)) continue;
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(h, &e)) {
            if (e.wFlags & PROCESS_HEAP_REGION) {
                t.committed += e.Region.dwCommittedSize;
            } else if (e.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) {
                continue;
            } else if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) {
                t.freeBytes += e.cbData;
                ++t.freeBlocks;
                t.largestFree = std::max<uint64_t>(t.largestFree, e.cbData);
            } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                t.busyBytes += e.cbData;
                ++t.busyBlocks;
                if (e.cbData >= kLargeBlock) t.committed += e.cbData;
                if (sizes) {
                    auto& s = bySize[e.cbData];
                    ++s.first;
                    s.second += e.cbData;
                }
            }
        }
        HeapUnlock(h);
        all.committed += t.committed;
        all.busyBytes += t.busyBytes;
        all.busyBlocks += t.busyBlocks;
        ObjectBuilder hb;
        hb.set("committed", num(t.committed));
        hb.set("busyBytes", num(t.busyBytes));
        hb.set("busyBlocks", num(t.busyBlocks));
        hb.set("freeBytes", num(t.freeBytes));
        hb.set("freeBlocks", num(t.freeBlocks));
        hb.set("largestFree", num(t.largestFree));
        list.set(ev::setElement(list.get(), idx++, hb.get()));
    }
    out.set("privateCommitted", num(privateCommitted));
    {
        uint64_t threads = 0;
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
        out.set("threads", num(threads));
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        out.set("handles", num(handles));
    }
    out.set("executableCommitted", num(executableCommitted));
    out.set("privateRegions", num(privateRegions));
    out.set("imageCommitted", num(imageCommitted));
    out.set("mappedCommitted", num(mappedCommitted));
    out.set("heapCommitted", num(all.committed));
    out.set("heapBusyBytes", num(all.busyBytes));
    out.set("heapBusyBlocks", num(all.busyBlocks));
    out.set("outsideHeaps", num(privateCommitted > all.committed ? privateCommitted - all.committed : 0));
    out.set("heaps", list.get());
    if (sizes) {
        ev::Persistent arr(ev::makeArray(0));
        uint32_t i = 0;
        for (const auto& [size, cb] : bySize) {
            ObjectBuilder row;
            row.set("size", num(size));
            row.set("count", num(cb.first));
            row.set("bytes", num(cb.second));
            arr.set(ev::setElement(arr.get(), i++, row.get()));
        }
        out.set("sizes", arr.get());
        ev::Persistent regs(ev::makeArray(0));
        uint32_t ri = 0;
        for (const auto& [key, count] : regionKinds) {
            ObjectBuilder row;
            row.set("protect", num(key >> 40));
            row.set("size", num(key & ((uint64_t{1} << 40) - 1)));
            row.set("count", num(count));
            regs.set(ev::setElement(regs.get(), ri++, row.get()));
        }
        out.set("regions", regs.get());
    }
#else
    (void)sizes;
#endif
    const bronze::embed::RuntimeTelemetry tel = bronze::embed::getRuntimeTelemetry();
    out.set("jsHeapCommitted", num(tel.heapCommittedBytes));
    out.set("skiaFontCache", num(SkGraphics::GetFontCacheUsed()));
    out.set("skiaResourceCache", num(SkGraphics::GetResourceCacheTotalBytesUsed()));
    // The decoded-image store (<img>, CSS images, createImageBitmap(blob)):
    // what its cache holds, and the budget past which it evicts.
    {
        render::ImageStore& store = render::ImageStore::instance();
        ObjectBuilder images;
        images.set("cachedBytes", num(store.cachedBytes()));
        images.set("cachedCount", num(store.cachedCount()));
        images.set("budgetBytes", num(store.budgetBytes()));
        out.set("imageStore", images.get());
    }
    // GPU memory: Ganesh's resource cache and the device pool. Device-only
    // memory is not the process's; host-visible pool memory is (mapped).
    const render::SkiaGpu::MemoryStats g = render::SkiaGpu::targetMemoryStats();
    if (g.valid) {
        ObjectBuilder gpu;
        gpu.set("ganeshBytes", num(g.ganeshBytes));
        gpu.set("ganeshPurgeableBytes", num(g.ganeshPurgeableBytes));
        gpu.set("ganeshLimit", num(g.ganeshLimit));
        gpu.set("ganeshAllocatorBytes", num(g.ganeshVmaAllocated));
        gpu.set("ganeshAllocatorUsedBytes", num(g.ganeshVmaUsed));
        gpu.set("ganeshCount", num(static_cast<uint64_t>(g.ganeshCount)));
        gpu.set("poolHostVisibleBytes", num(g.poolHostVisibleBytes));
        gpu.set("poolDeviceOnlyBytes", num(g.poolDeviceOnlyBytes));
        gpu.set("poolAllocations", num(g.poolAllocations));
        gpu.set("poolBlocks", num(g.poolBlocks));
        gpu.set("poolDedicated", num(g.poolDedicated));
        gpu.set("uploadTexturesLive", num(g.uploadsLive));
        out.set("gpu", gpu.get());
    }
    return out.get();
}

}  // namespace bro::bronze_host
