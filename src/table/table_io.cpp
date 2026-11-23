#include "table/table_io.h"

#include <cstring>
#include <fstream>
#include <limits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bro::table {

namespace {

constexpr char kMagic[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '1'};
constexpr uint32_t kFormatVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t colCount;
    uint32_t chunkCount;
    uint32_t reserved;
    uint64_t totalAliveRows;
    uint32_t dictCount;
    uint32_t reserved2;
    uint64_t payloadBytes;
};
#pragma pack(pop)

void appendU8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendU64(std::vector<uint8_t>& buf, uint64_t v) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void padToAlign64(std::vector<uint8_t>& buf) {
    const size_t rem = buf.size() & 63u;
    if (rem != 0) buf.resize(buf.size() + (64u - rem), 0);
}

struct MappedFileOwner {
    const uint8_t* data = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    HANDLE mapHandle = nullptr;
#else
    int fd = -1;
#endif
    ~MappedFileOwner() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapHandle) CloseHandle(mapHandle);
        if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
#else
        if (data && size > 0) munmap(const_cast<uint8_t*>(data), size);
        if (fd >= 0) close(fd);
#endif
    }
};

struct ByteCursor {
    const uint8_t* ptr = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool has(size_t n) const { return pos <= size && n <= (size - pos); }

    template <typename T>
    bool read(T& out) {
        if (!has(sizeof(T))) return false;
        std::memcpy(&out, ptr + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool skipToAlign64() {
        const size_t rem = pos & 63u;
        if (rem == 0) return true;
        const size_t add = 64u - rem;
        if (!has(add)) return false;
        pos += add;
        return true;
    }
};

bool parseHeaderAndDirectory(ByteCursor& cur, std::vector<ColumnDef>& outCols,
                             std::shared_ptr<StringDictionary>& outDict,
                             uint32_t& outChunkCount, uint64_t& outAliveRows,
                             std::string* err) {
    FileHeader hdr{};
    if (!cur.read(hdr)) {
        if (err) *err = "truncated .brotable header";
        return false;
    }
    if (std::memcmp(hdr.magic, kMagic, 8) != 0 || hdr.version != kFormatVersion) {
        if (err) *err = "invalid .brotable magic or version";
        return false;
    }
    if (hdr.payloadBytes > cur.size || hdr.colCount > 4096) {
        if (err) *err = "corrupted .brotable header bounds";
        return false;
    }
    outCols.reserve(hdr.colCount);
    for (uint32_t c = 0; c < hdr.colCount; ++c) {
        uint32_t nameLen = 0;
        uint8_t typeByte = 0;
        if (!cur.read(nameLen) || !cur.read(typeByte) || typeByte > static_cast<uint8_t>(ColType::String) ||
            !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable column schema";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        cur.pos += nameLen;
        outCols.push_back(ColumnDef{std::move(name), static_cast<ColType>(typeByte)});
    }
    outDict = std::make_shared<StringDictionary>();
    for (uint32_t i = 0; i < hdr.dictCount; ++i) {
        uint32_t len = 0;
        if (!cur.read(len) || !cur.has(len)) {
            if (err) *err = "corrupted .brotable dictionary";
            return false;
        }
        std::string_view sv(reinterpret_cast<const char*>(cur.ptr + cur.pos), len);
        cur.pos += len;
        outDict->intern(sv);
    }
    outChunkCount = hdr.chunkCount;
    outAliveRows = hdr.totalAliveRows;
    return true;
}

std::shared_ptr<TableSnapshot> parseSnapshotImpl(const uint8_t* data, size_t size,
                                                 std::shared_ptr<const void> owner,
                                                 std::string* err) {
    if (!data || size < sizeof(FileHeader)) {
        if (err) *err = "empty or truncated .brotable data";
        return nullptr;
    }
    ByteCursor cur{data, size, 0};
    std::vector<ColumnDef> cols;
    std::shared_ptr<StringDictionary> dict;
    uint32_t chunkCount = 0;
    uint64_t totalAlive = 0;
    if (!parseHeaderAndDirectory(cur, cols, dict, chunkCount, totalAlive, err)) return nullptr;

    auto snap = std::make_shared<TableSnapshot>();
    for (auto& cd : cols) snap->schema.addColumn(std::move(cd.name), cd.type);
    snap->schemaHash = snap->schema.computeHash();
    snap->dict = std::move(dict);
    snap->totalAliveRows = 0;
    snap->version = 1;
    snap->structureVersion = 1;
    snap->chunks.reserve(chunkCount);

    for (uint32_t ci = 0; ci < chunkCount; ++ci) {
        uint32_t rowCount = 0;
        uint32_t aliveCount = 0;
        std::array<uint64_t, kChunkAliveWords> aliveWords{};
        if (!cur.read(rowCount) || !cur.read(aliveCount) || rowCount > kChunkRows ||
            aliveCount > rowCount || !cur.read(aliveWords)) {
            if (err) *err = "corrupted .brotable chunk metadata";
            return nullptr;
        }
        TableSnapshot::FrozenChunk fc;
        fc.chunkId = ci + 1;
        fc.dirtyVersion = 1;
        fc.rowCount = rowCount;
        fc.aliveCount = aliveCount;
        fc.aliveWords = aliveWords;
        fc.columns.reserve(cols.size());
        snap->totalAliveRows += aliveCount;

        for (size_t c = 0; c < cols.size(); ++c) {
            uint64_t fixedBytes = 0;
            uint64_t strBytes = 0;
            if (!cur.read(fixedBytes) || !cur.read(strBytes) || !cur.skipToAlign64() ||
                !cur.has(static_cast<size_t>(fixedBytes))) {
                if (err) *err = "corrupted .brotable column payload";
                return nullptr;
            }
            const uint8_t* fixedPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(fixedBytes);
            if (!cur.has(static_cast<size_t>(strBytes))) {
                if (err) *err = "corrupted .brotable string arena";
                return nullptr;
            }
            const uint8_t* strPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(strBytes);

            if (owner) {
                fc.columns.push_back(ColumnChunk::createMapped(
                    cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                    strPtr, static_cast<size_t>(strBytes), owner));
            } else {
                auto nonOwning = std::shared_ptr<const void>(fixedPtr ? fixedPtr : data, [](const void*) {});
                auto owned = ColumnChunk::createMapped(
                                 cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                                 strPtr, static_cast<size_t>(strBytes), std::move(nonOwning))
                                 ->clone();
                fc.columns.push_back(std::move(owned));
            }
        }
        snap->chunks.push_back(std::move(fc));
    }
    return snap;
}

}  // namespace

std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap) {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(FileHeader) + snap.rowCount() * 16 + 256);

    FileHeader hdr{};
    std::memcpy(hdr.magic, kMagic, 8);
    hdr.version = kFormatVersion;
    hdr.colCount = static_cast<uint32_t>(snap.schema.size());
    hdr.chunkCount = static_cast<uint32_t>(snap.chunks.size());
    hdr.totalAliveRows = snap.totalAliveRows;
    hdr.dictCount = snap.dict ? static_cast<uint32_t>(snap.dict->size()) : 0u;
    buf.resize(sizeof(FileHeader));

    for (const ColumnDef& cd : snap.schema.columns) {
        appendU32(buf, static_cast<uint32_t>(cd.name.size()));
        appendU8(buf, static_cast<uint8_t>(cd.type));
        buf.insert(buf.end(), cd.name.begin(), cd.name.end());
    }
    if (snap.dict) {
        const uint32_t n = static_cast<uint32_t>(snap.dict->size());
        for (uint32_t id = 0; id < n; ++id) {
            std::string s = snap.dict->get(id);
            appendU32(buf, static_cast<uint32_t>(s.size()));
            buf.insert(buf.end(), s.begin(), s.end());
        }
    }

    for (const auto& fc : snap.chunks) {
        appendU32(buf, fc.rowCount);
        appendU32(buf, fc.aliveCount);
        const size_t off = buf.size();
        buf.resize(off + sizeof(fc.aliveWords));
        std::memcpy(buf.data() + off, fc.aliveWords.data(), sizeof(fc.aliveWords));

        for (size_t c = 0; c < snap.schema.size(); ++c) {
            const ColumnChunk& col = *fc.columns[c];
            const size_t fixedBytes = static_cast<size_t>(fc.rowCount) * colTypeElemSize(col.type());
            const size_t strBytes = (col.type() == ColType::String) ? col.stringArenaSize() : 0u;
            appendU64(buf, fixedBytes);
            appendU64(buf, strBytes);
            padToAlign64(buf);
            if (fixedBytes > 0) {
                const uint8_t* raw = col.rawData();
                buf.insert(buf.end(), raw, raw + fixedBytes);
            }
            if (strBytes > 0) {
                const uint8_t* strRaw = col.stringArenaData();
                buf.insert(buf.end(), strRaw, strRaw + strBytes);
            }
        }
    }

    hdr.payloadBytes = buf.size();
    std::memcpy(buf.data(), &hdr, sizeof(FileHeader));
    return buf;
}

std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError) {
    // Parse with nullptr owner triggers clone() cleanly inside parseSnapshotImpl? No:
    // keep a lightweight non-owning dummy owner pointing to caller buffer for parseSnapshotImpl, then Table::fromSnapshot clones immediately!
    auto dummyOwner = std::shared_ptr<const void>(data, [](const void*) {});
    auto snap = parseSnapshotImpl(data, size, dummyOwner, outError);
    if (!snap) return nullptr;
    return Table::fromSnapshot(*snap);
}

bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        std::string* outError) {
    std::vector<uint8_t> bytes = serializeSnapshotBytes(snap);
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outError) *outError = "failed to open file for writing: " + path;
        return false;
    }
    size_t writtenTotal = 0;
    while (writtenTotal < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - writtenTotal, 64u * 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + writtenTotal, chunk, &written, nullptr) ||
            written != chunk) {
            CloseHandle(h);
            if (outError) *outError = "failed writing .brotable file: " + path;
            return false;
        }
        writtenTotal += written;
    }
    CloseHandle(h);
    return true;
#else
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (outError) *outError = "failed to open file for writing: " + path;
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!ofs) {
        if (outError) *outError = "failed writing .brotable file: " + path;
        return false;
    }
    return true;
#endif
}

std::shared_ptr<Table> loadTableFromFile(const std::string& path, std::string* outError) {
    auto snap = mmapSnapshotFromFile(path, outError);
    if (!snap) return nullptr;
    return Table::fromSnapshot(*snap);
}

std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError) {
    auto owner = std::make_shared<MappedFileOwner>();
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    owner->fileHandle = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (owner->fileHandle == INVALID_HANDLE_VALUE) {
        if (outError) *outError = "failed to open .brotable file: " + path;
        return nullptr;
    }
    LARGE_INTEGER fsz{};
    if (!GetFileSizeEx(owner->fileHandle, &fsz) || fsz.QuadPart <= 0) {
        if (outError) *outError = "empty or unreadable .brotable file: " + path;
        return nullptr;
    }
    owner->size = static_cast<size_t>(fsz.QuadPart);
    owner->mapHandle = CreateFileMappingW(owner->fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!owner->mapHandle) {
        if (outError) *outError = "CreateFileMappingW failed for: " + path;
        return nullptr;
    }
    void* view = MapViewOfFile(owner->mapHandle, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        if (outError) *outError = "MapViewOfFile failed for: " + path;
        return nullptr;
    }
    owner->data = static_cast<const uint8_t*>(view);
#else
    owner->fd = open(path.c_str(), O_RDONLY);
    if (owner->fd < 0) {
        if (outError) *outError = "failed to open .brotable file: " + path;
        return nullptr;
    }
    struct stat st{};
    if (fstat(owner->fd, &st) != 0 || st.st_size <= 0) {
        if (outError) *outError = "empty or unreadable .brotable file: " + path;
        return nullptr;
    }
    owner->size = static_cast<size_t>(st.st_size);
    void* view = mmap(nullptr, owner->size, PROT_READ, MAP_PRIVATE, owner->fd, 0);
    if (view == MAP_FAILED) {
        if (outError) *outError = "mmap failed for: " + path;
        return nullptr;
    }
    owner->data = static_cast<const uint8_t*>(view);
#endif
    return parseSnapshotImpl(owner->data, owner->size, owner, outError);
}

}  // namespace bro::table
