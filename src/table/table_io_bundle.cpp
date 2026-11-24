#include "table/table_io_bundle.h"

#include "table/table_io_internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bro::table {

namespace {

using namespace io_internal;

constexpr char kBundleMagic[8] = {'B', 'R', 'O', 'B', 'N', 'D', '0', '1'};
constexpr uint32_t kBundleVersion = 1;
constexpr uint64_t kBundlePageAlign = 4096ULL;
constexpr size_t kBundleBufBytes = 4u * 1024u * 1024u;

#pragma pack(push, 1)
struct BundleHeader {
    char magic[8];
    uint32_t version;
    uint32_t tableCount;
    uint32_t flags;
    uint32_t reserved;
    uint64_t dirOffset;
    uint64_t totalBytes;
};
#pragma pack(pop)

static_assert(sizeof(BundleHeader) == 40, "BundleHeader must be 40 bytes");

#ifdef _WIN32
std::wstring bundleUtf8ToWide(const std::string& s) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), wlen);
    return ws;
}
#endif

std::atomic<uint64_t> g_bundleTmpSeq{0};

class BundleRawWriter {
public:
    BundleRawWriter(const std::string& finalPath, const std::string& tmpPath)
        : finalPath_(finalPath), tmpPath_(tmpPath), buf_(kBundleBufBytes) {
#ifdef _WIN32
        wtmp_ = bundleUtf8ToWide(tmpPath_);
        wfinal_ = bundleUtf8ToWide(finalPath_);
        h_ = CreateFileW(wtmp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) ok_ = false, err_ = "failed creating bundle temp file: " + tmpPath_;
#else
        fd_ = open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) ok_ = false, err_ = "failed creating bundle temp file: " + tmpPath_;
#endif
    }

    ~BundleRawWriter() { abortCleanup(); }

    uint64_t filePos() const { return totalWritten_; }
    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }

    void write(const void* data, size_t size) {
        if (!ok_ || size == 0) return;
        const auto* src = static_cast<const uint8_t*>(data);
        totalWritten_ += size;
        if (size >= buf_.size()) {
            flush();
            if (ok_) writeAllRaw(src, size);
            return;
        }
        if (bufPos_ + size > buf_.size()) flush();
        if (!ok_) return;
        std::memcpy(buf_.data() + bufPos_, src, size);
        bufPos_ += size;
    }

    void padTo4096() {
        const size_t rem = static_cast<size_t>(totalWritten_ & (kBundlePageAlign - 1ULL));
        if (rem == 0) return;
        std::vector<uint8_t> zeros(static_cast<size_t>(kBundlePageAlign) - rem, 0);
        write(zeros.data(), zeros.size());
    }

    bool writeAtOffset(uint64_t off, const void* data, size_t size) {
        flush();
        if (!ok_) return false;
#ifdef _WIN32
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(off);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) {
            ok_ = false;
            err_ = "failed seeking in bundle temp file";
            return false;
        }
        const bool wrote = writeAllRaw(static_cast<const uint8_t*>(data), size);
        LARGE_INTEGER endLi{};
        endLi.QuadPart = static_cast<LONGLONG>(totalWritten_);
        if (wrote && !SetFilePointerEx(h_, endLi, nullptr, FILE_BEGIN)) {
            ok_ = false;
            err_ = "failed restoring bundle temp file pointer";
            return false;
        }
        return wrote;
#else
        if (lseek(fd_, static_cast<off_t>(off), SEEK_SET) < 0) {
            ok_ = false;
            err_ = "failed seeking in bundle temp file";
            return false;
        }
        const bool wrote = writeAllRaw(static_cast<const uint8_t*>(data), size);
        if (wrote && lseek(fd_, static_cast<off_t>(totalWritten_), SEEK_SET) < 0) {
            ok_ = false;
            err_ = "failed restoring bundle temp file pointer";
            return false;
        }
        return wrote;
#endif
    }

    bool commit() {
        flush();
        if (!ok_) { abortCleanup(); return false; }
#ifdef _WIN32
        FlushFileBuffers(h_);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(wtmp_.c_str(), wfinal_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(wtmp_.c_str());
            err_ = "failed atomically committing bundle file: " + finalPath_;
            ok_ = false;
            return false;
        }
#else
        fsync(fd_);
        close(fd_);
        fd_ = -1;
        if (rename(tmpPath_.c_str(), finalPath_.c_str()) != 0) {
            unlink(tmpPath_.c_str());
            err_ = "failed atomically committing bundle file: " + finalPath_;
            ok_ = false;
            return false;
        }
#endif
        committed_ = true;
        return true;
    }

private:
    void flush() {
        if (!ok_ || bufPos_ == 0) return;
        writeAllRaw(buf_.data(), bufPos_);
        bufPos_ = 0;
    }

    bool writeAllRaw(const uint8_t* src, size_t size) {
        size_t off = 0;
        while (off < size) {
#ifdef _WIN32
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - off, 64u * 1024u * 1024u));
            DWORD written = 0;
            if (!WriteFile(h_, src + off, chunk, &written, nullptr) || written != chunk) {
                ok_ = false;
                err_ = "failed writing bundle temp file";
                return false;
            }
            off += written;
#else
            ssize_t n = ::write(fd_, src + off, size - off);
            if (n <= 0) {
                ok_ = false;
                err_ = "failed writing bundle temp file";
                return false;
            }
            off += static_cast<size_t>(n);
#endif
        }
        return true;
    }

    void abortCleanup() {
        if (committed_) return;
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
            DeleteFileW(wtmp_.c_str());
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
            unlink(tmpPath_.c_str());
        }
#endif
    }

    std::string finalPath_, tmpPath_, err_;
    std::vector<uint8_t> buf_;
    size_t bufPos_ = 0;
    uint64_t totalWritten_ = 0;
    bool ok_ = true;
    bool committed_ = false;
#ifdef _WIN32
    std::wstring wtmp_, wfinal_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

class MemberSubSink final : public ByteSink {
public:
    explicit MemberSubSink(BundleRawWriter& w) : writer_(w) {}

protected:
    void writeBytesRaw(const void* data, size_t size) override {
        writer_.write(data, size);
        if (!writer_.ok()) fail(writer_.error());
    }

private:
    BundleRawWriter& writer_;
};

struct DirRecord {
    std::string name;
    uint64_t offset = 0;
    uint64_t length = 0;
};

bool parseBundleDirectory(const uint8_t* data, size_t size,
                          std::vector<DirRecord>& outDir,
                          uint32_t& outFlags,
                          std::string* err) {
    if (!data || size < sizeof(BundleHeader)) {
        if (err) *err = "truncated .brotable bundle header";
        return false;
    }
    BundleHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (std::memcmp(hdr.magic, kBundleMagic, 8) != 0 || hdr.version != kBundleVersion) {
        if (err) *err = "invalid .brotable bundle magic or version";
        return false;
    }
    if (hdr.totalBytes != size || hdr.dirOffset < sizeof(BundleHeader) || hdr.dirOffset > size) {
        if (err) *err = "corrupted .brotable bundle directory offset";
        return false;
    }
    outFlags = hdr.flags;
    ByteCursor cur{data, size, static_cast<size_t>(hdr.dirOffset), nullptr};
    outDir.reserve(hdr.tableCount);
    for (uint32_t i = 0; i < hdr.tableCount; ++i) {
        uint32_t nameLen = 0;
        if (!cur.read(nameLen) || nameLen == 0 || nameLen > 65536 || !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable bundle entry name";
            return false;
        }
        DirRecord rec;
        rec.name.assign(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        cur.pos += nameLen;
        if (!cur.read(rec.offset) || !cur.read(rec.length) ||
            rec.offset > size || rec.length > size - rec.offset) {
            if (err) *err = "corrupted .brotable bundle entry bounds";
            return false;
        }
        outDir.push_back(std::move(rec));
    }
    if (cur.pos != size) {
        if (err) *err = "trailing bytes after .brotable bundle directory";
        return false;
    }
    return true;
}

}  // namespace

bool saveBundleToFile(const std::vector<BundleEntryInput>& entries,
                      const std::string& path,
                      const SaveOptions& opts,
                      std::string* outError) {
    std::unordered_set<std::string> seenNames;
    for (const auto& e : entries) {
        if (e.name.empty() || !e.snapshot || !seenNames.insert(e.name).second) {
            if (outError) *outError = "bundle entries must have unique non-empty names and valid snapshots";
            return false;
        }
    }

    const uint64_t seq = g_bundleTmpSeq.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const uint64_t pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
    const uint64_t pid = static_cast<uint64_t>(getpid());
#endif
    const std::string tmpPath = path + ".bundle.tmp." + std::to_string(pid) + "." + std::to_string(seq);

    BundleRawWriter writer(path, tmpPath);
    BundleHeader hdr{};
    std::memcpy(hdr.magic, kBundleMagic, 8);
    hdr.version = kBundleVersion;
    hdr.tableCount = static_cast<uint32_t>(entries.size());
    hdr.flags = opts.compress ? kFlagLz4Compressed : 0u;
    writer.write(&hdr, sizeof(hdr));

    std::vector<DirRecord> dir;
    dir.reserve(entries.size());
    for (const auto& e : entries) {
        writer.padTo4096();
        const uint64_t memberStart = writer.filePos();
        MemberSubSink sub(writer);
        FileHeaderV3 subHdr{};
        if (!writeSnapshotStream(*e.snapshot, sub, opts, subHdr)) {
            if (outError) *outError = sub.error().empty() ? writer.error() : sub.error();
            return false;
        }
        const uint64_t memberLen = sub.bytesWritten();
        if (!writer.writeAtOffset(memberStart, &subHdr, sizeof(subHdr))) {
            if (outError) *outError = writer.error();
            return false;
        }
        // Restore file position conceptually by writing only forward after seek? Notice BundleRawWriter's
        // writeAtOffset left file pointer at memberStart + sizeof(subHdr)! Seek back to totalWritten_!
        // Let's maintain sequential write pointer explicitly cleanly right below:
        dir.push_back(DirRecord{e.name, memberStart, memberLen});
    }

    // Recompute/verify writeAtOffset restores end-of-file seek — let's check:
    // Calling writeAtOffset left OS file pointer at memberStart + sizeof(FileHeaderV3)!
    // Let's handle sequential writes cleanly via member buffers OR restoring OS pointer in writeAtOffset.
    if (!writer.ok()) {
        if (outError) *outError = writer.error();
        return false;
    }

    hdr.dirOffset = writer.filePos();
    for (const DirRecord& rec : dir) {
        const uint32_t nl = static_cast<uint32_t>(rec.name.size());
        writer.write(&nl, sizeof(nl));
        writer.write(rec.name.data(), nl);
        writer.write(&rec.offset, sizeof(rec.offset));
        writer.write(&rec.length, sizeof(rec.length));
    }
    hdr.totalBytes = writer.filePos();

    if (!writer.writeAtOffset(0, &hdr, sizeof(hdr)) || !writer.commit()) {
        if (outError) *outError = writer.error();
        return false;
    }
    return true;
}

bool loadBundleFromFile(const std::string& path,
                        std::vector<LoadedBundleEntry>& outEntries,
                        std::string* outError) {
    outEntries.clear();
    const uint8_t* data = nullptr;
    size_t size = 0;
    auto owner = mapReadOnlyFileShared(path, data, size, outError);
    if (!owner) return false;

    std::vector<DirRecord> dir;
    uint32_t flags = 0;
    if (!parseBundleDirectory(data, size, dir, flags, outError)) return false;

    outEntries.reserve(dir.size());
    for (const DirRecord& rec : dir) {
        PersistedIndexSpecs specs;
        uint32_t fmtVer = 0;
        auto snap = parseSnapshotRange(data + rec.offset, static_cast<size_t>(rec.length),
                                       owner, &specs, true, true, &fmtVer, outError);
        if (!snap) {
            outEntries.clear();
            return false;
        }
        auto tbl = Table::fromSnapshot(*snap);
        if (fmtVer < kFormatVersionV3) applyPersistedIndexes(*tbl, specs);
        outEntries.push_back(LoadedBundleEntry{rec.name, std::move(tbl)});
    }
    return true;
}

bool mmapBundleFromFile(const std::string& path,
                        std::vector<MappedBundleEntry>& outEntries,
                        std::string* outError,
                        bool verifyChecksum) {
    outEntries.clear();
    const uint8_t* data = nullptr;
    size_t size = 0;
    auto owner = mapReadOnlyFileShared(path, data, size, outError);
    if (!owner) return false;

    std::vector<DirRecord> dir;
    uint32_t flags = 0;
    if (!parseBundleDirectory(data, size, dir, flags, outError)) return false;
    if ((flags & kFlagLz4Compressed) != 0) {
        if (outError) *outError = "compressed .brotable bundle cannot be zero-copy mmapped; use loadBundle instead";
        return false;
    }

    outEntries.reserve(dir.size());
    for (const DirRecord& rec : dir) {
        if ((rec.offset & 63ULL) != 0) {
            if (outError) *outError = "bundle member '" + rec.name + "' is not 64-byte aligned";
            outEntries.clear();
            return false;
        }
        PersistedIndexSpecs specs;
        uint32_t fmtVer = 0;
        auto snap = parseSnapshotRange(data + rec.offset, static_cast<size_t>(rec.length),
                                       owner, &specs, verifyChecksum, false, &fmtVer, outError);
        if (!snap) {
            outEntries.clear();
            return false;
        }
        if (fmtVer < kFormatVersionV3) applyPersistedIndexesToSnapshot(*snap, specs);
        outEntries.push_back(MappedBundleEntry{rec.name, std::move(snap)});
    }
    return true;
}

}  // namespace bro::table
