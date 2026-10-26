#include "engine/install_record.h"

#include "util/json_in.h"
#include "util/json_out.h"
#include "util/sha.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

fs::path asReached(const std::string& dir) {
    std::error_code ec;
    fs::path p = fs::absolute(u8path(dir), ec).lexically_normal();
    if (!p.has_filename()) p = p.parent_path();
    return p;
}

std::string fileSha256(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    util::Sha256 h;
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n > 0) h.update(buf.data(), static_cast<size_t>(n));
    }
    return h.hex();
}

int64_t mtimeTicks(const fs::directory_entry& e) {
    std::error_code ec;
    const auto t = e.last_write_time(ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

// Every regular file under `root`, by relative path (links followed, as the
// app sees them). The stat of each comes from the directory listing.
std::map<std::string, fs::directory_entry> listFiles(const fs::path& root) {
    std::map<std::string, fs::directory_entry> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        out.emplace(u8str(fs::relative(it->path(), root, fec)), *it);
    }
    return out;
}

std::string treeHash(const std::vector<InstallRecordFile>& files) {
    util::Sha256 h;
    const std::string tag = "bro-install/1\n";
    h.update(tag.data(), tag.size());
    for (const InstallRecordFile& f : files) {
        const std::string line = f.path + '\0' + std::to_string(f.size) + '\0' + f.sha256 + '\n';
        h.update(line.data(), line.size());
    }
    return h.hex();
}

}  // namespace

std::string installRecordPath(const std::string& installedDir) {
    const fs::path p = asReached(installedDir);
    return u8str(p.parent_path() / u8path(u8str(p.filename()) + ".install.json"));
}

std::string hashInstalledTree(const std::string& dir, std::vector<InstallRecordFile>* filesOut) {
    std::error_code ec;
    const fs::path root = fs::canonical(asReached(dir), ec);
    std::vector<InstallRecordFile> files;
    if (!ec) {
        for (auto& [rel, entry] : listFiles(root)) {
            InstallRecordFile f;
            f.path = rel;
            std::error_code sec;
            f.size = entry.file_size(sec);
            f.mtime = mtimeTicks(entry);
            f.sha256 = fileSha256(entry.path());
            files.push_back(std::move(f));
        }
    }
    const std::string hash = treeHash(files);
    if (filesOut) *filesOut = std::move(files);
    return hash;
}

bool writeInstallRecord(const std::string& installedDir, const std::string& id, bool linked, std::string* error) {
    InstallRecord rec;
    rec.id = id;
    rec.linked = linked;
    rec.installedAt = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    if (linked) {
        std::error_code ec;
        rec.hash = "linked:" + u8str(fs::weakly_canonical(asReached(installedDir), ec));
    } else {
        rec.hash = hashInstalledTree(installedDir, &rec.files);
    }
    util::JsonOut j;
    j.beginObject();
    j.key("id").string(rec.id);
    j.key("hash").string(rec.hash);
    j.key("linked").boolean(rec.linked);
    j.key("installedAt").number(rec.installedAt, 0);
    j.key("files").beginArray();
    for (const InstallRecordFile& f : rec.files) {
        j.beginObject();
        j.key("path").string(f.path);
        j.key("size").integer(static_cast<int64_t>(f.size));
        j.key("mtime").integer(f.mtime);
        j.key("sha256").string(f.sha256);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    const fs::path path = u8path(installRecordPath(installedDir));
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
        out << j.str() << "\n";
        if (!out) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "cannot replace " + u8str(path) + ": " + ec.message();
        return false;
    }
    return true;
}

bool readInstallRecord(const std::string& installedDir, InstallRecord& out) {
    std::ifstream f(u8path(installRecordPath(installedDir)), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    util::JsonValue root;
    if (!util::parseJson(ss.str(), root) || !root.isObject()) return false;
    out = {};
    out.id = root.get("id").string;
    out.hash = root.get("hash").string;
    out.linked = root.get("linked").type == util::JsonValue::Type::Bool && root.get("linked").boolean;
    out.installedAt = root.get("installedAt").number;
    if (const util::JsonValue* files = root.find("files"); files && files->isArray()) {
        for (const util::JsonValue& v : files->array) {
            if (!v.isObject()) continue;
            InstallRecordFile rf;
            rf.path = v.get("path").string;
            rf.size = static_cast<uint64_t>(v.get("size").number);
            rf.mtime = static_cast<int64_t>(v.get("mtime").number);
            rf.sha256 = v.get("sha256").string;
            out.files.push_back(std::move(rf));
        }
    }
    return !out.hash.empty();
}

void removeInstallRecord(const std::string& installedDir) {
    std::error_code ec;
    fs::remove(u8path(installRecordPath(installedDir)), ec);
}

InstallCheck checkInstall(const std::string& installedDir) {
    const auto t0 = std::chrono::steady_clock::now();
    InstallCheck c;
    InstallRecord rec;
    if (!readInstallRecord(installedDir, rec)) {
        c.hash = "unrecorded";
    } else {
        c.recorded = true;
        if (rec.linked) {
            std::error_code ec;
            c.hash = "linked:" + u8str(fs::weakly_canonical(asReached(installedDir), ec));
            c.matches = c.hash == rec.hash;
        } else {
            std::error_code ec;
            const fs::path root = fs::canonical(asReached(installedDir), ec);
            bool same = !ec;
            if (same) {
                // mtime in the record is the file system's ticks as the
                // directory listing reported them; JSON numbers hold them
                // exactly to 2^53, which ticks of 100 ns reach in the year
                // 30000, so equality is a fair test.
                const auto now = listFiles(root);
                same = now.size() == rec.files.size();
                for (const InstallRecordFile& f : rec.files) {
                    if (!same) break;
                    auto it = now.find(f.path);
                    std::error_code sec;
                    same = it != now.end() && it->second.file_size(sec) == f.size &&
                           static_cast<double>(mtimeTicks(it->second)) == static_cast<double>(f.mtime);
                }
            }
            if (same) {
                c.hash = rec.hash;
                c.matches = true;
            } else {
                c.hashed = true;
                c.hash = hashInstalledTree(installedDir);
                c.matches = c.hash == rec.hash;
            }
        }
    }
    c.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return c;
}

}  // namespace bro::engine
