#include "util/model_fetch.h"

#include "util/json_in.h"
#include "util/sha.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef BRO_HAVE_CURL
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace bro::util {

const char* defaultModelSources() {
    return "wlejon/brovisionml-data wlejon/brosoundml-data";
}

std::vector<ModelSource> parseModelSources(const std::string& setting) {
    std::string text = setting;
    if (text.find_first_not_of(" \t\r\n,") == std::string::npos) text = defaultModelSources();
    std::vector<ModelSource> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n' || text[i] == ',')) ++i;
        size_t j = i;
        while (j < text.size() && !(text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n' || text[j] == ',')) ++j;
        if (j == i) break;
        std::string entry = text.substr(i, j - i);
        i = j;
        while (!entry.empty() && entry.back() == '/') entry.pop_back();
        ModelSource s;
        s.label = entry;
        const size_t scheme = entry.find("://");
        if (scheme != std::string::npos) {
            const size_t ds = entry.find("/datasets/", scheme + 3);
            if (ds == std::string::npos) continue;
            s.endpoint = entry.substr(0, ds);
            s.repo = entry.substr(ds + 10);
        } else {
            s.endpoint = "https://huggingface.co";
            s.repo = entry;
        }
        // <owner>/<repo>: exactly one '/', neither half empty.
        const size_t slash = s.repo.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= s.repo.size() ||
            s.repo.find('/', slash + 1) != std::string::npos) continue;
        out.push_back(std::move(s));
    }
    return out;
}

std::string modelFetchStagingDir(const std::string& root, const std::string& name) {
    return (fs::path(root) / ".bro-fetch" / fs::path(name)).lexically_normal().make_preferred().string();
}

// ---------------------------------------------------------------------------

ModelFetchJob::ModelFetchJob(std::string name, std::string root, std::vector<ModelSource> sources, bool listOnly,
                             std::shared_ptr<ModelFetchJob> after)
    : name_(std::move(name)), root_(std::move(root)), sources_(std::move(sources)), listOnly_(listOnly),
      after_(std::move(after)) {
    th_ = std::thread([this] { run(); });
}

ModelFetchJob::~ModelFetchJob() {
    cancel();
    if (th_.joinable()) th_.join();
}

ModelFetchJob::Snapshot ModelFetchJob::snapshot() const {
    Snapshot s;
    s.done = done_.load();
    s.total = total_.load();
    s.files = files_.load();
    std::lock_guard<std::mutex> lk(mu_);
    s.state = state_;
    s.file = file_;
    s.source = source_;
    s.dir = dir_;
    s.error = error_;
    s.code = code_;
    return s;
}

void ModelFetchJob::setFile(const std::string& f) {
    std::lock_guard<std::mutex> lk(mu_);
    file_ = f;
}

void ModelFetchJob::finish(State st, std::string dir, std::string error, std::string code) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = st;
    dir_ = std::move(dir);
    error_ = std::move(error);
    code_ = std::move(code);
    file_.clear();
}

#ifndef BRO_HAVE_CURL

void ModelFetchJob::run() {
    finish(State::Failed, {}, "this build of bro has no HTTP client (libcurl)", "unsupported");
}

#else

namespace {

struct FileEntry {
    std::string path;    // relative to the dataset root, "<name>/..."
    uint64_t size = 0;
    std::string sha256;  // lfs.oid
    std::string sha1;    // git blob oid, when not LFS
};

std::string encodePath(const std::string& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : p) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (keep) out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Retry a transient failure this many times after the first try.
constexpr int kAttempts = 4;

bool sleepUnlessCancelled(const ModelFetchJob& job, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (job.cancelled()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !job.cancelled();
}

int progressCb(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const ModelFetchJob*>(p)->cancelled() ? 1 : 0;
}

void commonOpts(CURL* easy, const std::string& url, ModelFetchJob* job) {
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
    // A stalled transfer (under 1 byte/s for 60 s) is an error, retried.
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "bro-models");
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, job);
}

size_t toString(void* data, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<const char*>(data), size * nmemb);
    return size * nmemb;
}

enum class Got { Ok, NotFound, Transient, Fatal, Cancelled };

// GET a small text body (a listing).
Got getText(ModelFetchJob* job, const std::string& url, std::string& body, std::string& err) {
    CURL* easy = curl_easy_init();
    if (!easy) { err = "curl_easy_init failed"; return Got::Fatal; }
    body.clear();
    commonOpts(easy, url, job);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    const CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);
    if (job->cancelled()) return Got::Cancelled;
    if (rc != CURLE_OK) {
        // file:// sources answer a missing listing as a missing file.
        if (rc == CURLE_FILE_COULDNT_READ_FILE) return Got::NotFound;
        err = curl_easy_strerror(rc);
        return Got::Transient;
    }
    if (status == 404) return Got::NotFound;
    if (status >= 500 || status == 408 || status == 429) { err = "HTTP " + std::to_string(status); return Got::Transient; }
    if (status >= 400) { err = "HTTP " + std::to_string(status); return Got::Fatal; }
    return Got::Ok;
}

Got getTextRetrying(ModelFetchJob* job, const std::string& url, std::string& body, std::string& err) {
    Got g = Got::Fatal;
    for (int a = 0; a < kAttempts; ++a) {
        if (a > 0 && !sleepUnlessCancelled(*job, 250 << (a - 1))) return Got::Cancelled;
        g = getText(job, url, body, err);
        if (g != Got::Transient) return g;
    }
    return g;
}

// The listing of `dir` in `src`, recursively, into `files`.
Got listTree(ModelFetchJob* job, const ModelSource& src, const std::string& dir,
             std::vector<FileEntry>& files, std::string& err, int depth) {
    if (depth > 16) { err = "listing nested too deep"; return Got::Fatal; }
    const std::string url = src.endpoint + "/api/datasets/" + src.repo + "/tree/main/" + encodePath(dir);
    std::string body;
    const Got g = getTextRetrying(job, url, body, err);
    if (g != Got::Ok) return g;
    JsonValue v;
    std::string perr;
    if (!parseJson(body, v, &perr) || !v.isArray()) {
        // HF answers a path that is a file (or a missing one, on some
        // mirrors) with an object, not a folder listing.
        if (v.isObject()) return Got::NotFound;
        err = "unreadable listing from " + url + (perr.empty() ? "" : ": " + perr);
        return Got::Transient;
    }
    for (const JsonValue& e : v.array) {
        const std::string& type = e.get("type").string;
        const std::string& path = e.get("path").string;
        if (path.empty()) continue;
        if (type == "directory") {
            const Got sub = listTree(job, src, path, files, err, depth + 1);
            if (sub != Got::Ok && sub != Got::NotFound) return sub;
            continue;
        }
        if (type != "file") continue;
        FileEntry f;
        f.path = path;
        f.size = e.get("size").isNumber() ? static_cast<uint64_t>(e.get("size").number) : 0;
        const JsonValue& lfs = e.get("lfs");
        if (lfs.isObject() && lfs.get("oid").isString()) {
            f.sha256 = lfs.get("oid").string;
            if (lfs.get("size").isNumber()) f.size = static_cast<uint64_t>(lfs.get("size").number);
        } else {
            f.sha1 = e.get("oid").string;
        }
        files.push_back(std::move(f));
    }
    return Got::Ok;
}

// Hashes a file as the listing names it: sha256 of the bytes, or a git blob id.
struct Hasher {
    bool blob;
    Sha256 s256;
    Sha1 s1;
    Hasher(const FileEntry& f) : blob(f.sha256.empty()) {
        if (blob) {
            const std::string head = "blob " + std::to_string(f.size) + std::string(1, '\0');
            s1.update(head.data(), head.size());
        }
    }
    void update(const void* d, size_t n) { if (blob) s1.update(d, n); else s256.update(d, n); }
    bool matches(const FileEntry& f) {
        std::string want = blob ? f.sha1 : f.sha256;
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return (blob ? s1.hex() : s256.hex()) == want;
    }
};

struct Sink {
    ModelFetchJob* job;
    std::ofstream* out;
    Hasher* hasher;
    uint64_t written = 0;
    bool ioError = false;
};

size_t toFile(void* data, size_t size, size_t nmemb, void* userp) {
    auto* s = static_cast<Sink*>(userp);
    const size_t n = size * nmemb;
    if (s->job->cancelled()) return 0;
    s->out->write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!*s->out) { s->ioError = true; return 0; }
    s->hasher->update(data, n);
    s->written += n;
    s->job->addDone(n);
    return n;
}

// Hash the bytes already in `part` (a resumed file) into `h`.
bool hashExisting(const fs::path& part, uint64_t n, Hasher& h, ModelFetchJob* job) {
    std::ifstream in(part, std::ios::binary);
    if (!in) return false;
    std::vector<char> buf(1 << 20);
    uint64_t left = n;
    while (left > 0) {
        if (job->cancelled()) return false;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
        in.read(buf.data(), static_cast<std::streamsize>(take));
        if (in.gcount() != static_cast<std::streamsize>(take)) return false;
        h.update(buf.data(), take);
        left -= take;
    }
    return true;
}

struct FileResult {
    Got got = Got::Ok;
    std::string err;
    std::string code;  // for Fatal: "integrity" | "io" | "network"
};

// Download one file into `dest` (via `dest.part`), verified. The job's `done`
// counts this file's bytes on disk: it falls only when bytes are discarded.
FileResult fetchFile(ModelFetchJob* job, const ModelSource& src, const FileEntry& f, const fs::path& dest) {
    const fs::path part = fs::path(dest.string() + ".part");
    const std::string url = src.endpoint + "/datasets/" + src.repo + "/resolve/main/" + encodePath(f.path);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::string lastErr;
    uint64_t counted = 0;  // this file's bytes in the job's `done`
    auto account = [&](uint64_t n) { job->addDone(n - counted); counted = n; };  // wraps as a subtraction
    for (int a = 0; a < kAttempts; ++a) {
        if (a > 0 && !sleepUnlessCancelled(*job, 250 << (a - 1))) return {Got::Cancelled, {}, {}};
        if (job->cancelled()) return {Got::Cancelled, {}, {}};

        uint64_t have = 0;
        if (fs::exists(part, ec)) have = fs::file_size(part, ec);
        if (ec || have > f.size) { fs::remove(part, ec); have = 0; }

        Hasher hasher(f);
        if (have > 0 && !hashExisting(part, have, hasher, job)) {
            if (job->cancelled()) return {Got::Cancelled, {}, {}};
            fs::remove(part, ec);
            have = 0;
            hasher = Hasher(f);
        }
        account(have);

        bool transferred = true;
        uint64_t got = have;
        if (have < f.size || f.size == 0) {
            std::ofstream out(part, std::ios::binary | std::ios::app);
            if (!out) return {Got::Fatal, "cannot write " + part.string(), "io"};
            CURL* easy = curl_easy_init();
            if (!easy) return {Got::Fatal, "curl_easy_init failed", "io"};
            Sink sink{job, &out, &hasher};
            commonOpts(easy, url, job);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, toFile);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
            curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
            if (have > 0) curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(have));
            const CURLcode rc = curl_easy_perform(easy);
            long status = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_cleanup(easy);
            out.close();
            got = have + sink.written;
            counted = got;  // the sink added what it wrote
            if (job->cancelled()) return {Got::Cancelled, {}, {}};
            if (sink.ioError) return {Got::Fatal, "cannot write " + part.string() + " (disk full?)", "io"};
            if (rc != CURLE_OK) {
                transferred = false;
                if (rc == CURLE_RANGE_ERROR || status == 416) {
                    // The server will not resume: start this file over.
                    fs::remove(part, ec);
                    account(0);
                } else if (rc == CURLE_HTTP_RETURNED_ERROR && status >= 400 && status < 500 &&
                           status != 408 && status != 429) {
                    return {Got::Fatal, "HTTP " + std::to_string(status) + " for " + url, "network"};
                }
                lastErr = std::string(curl_easy_strerror(rc)) +
                          (status ? " (HTTP " + std::to_string(status) + ")" : std::string()) + " for " + url;
            }
        }
        if (!transferred || got != f.size) {
            if (transferred) lastErr = "short transfer of " + f.path + ": " + std::to_string(got) + " of " +
                                       std::to_string(f.size) + " bytes";
            continue;  // the next attempt resumes from what is on disk
        }
        if (!hasher.matches(f)) {
            fs::remove(part, ec);
            account(0);
            return {Got::Fatal,
                    f.path + " does not match the source's " + (f.sha256.empty() ? "git blob sha1 " + f.sha1
                                                                                : "sha256 " + f.sha256),
                    "integrity"};
        }
        fs::remove(dest, ec);
        fs::rename(part, dest, ec);
        if (ec) return {Got::Fatal, "cannot rename " + part.string() + ": " + ec.message(), "io"};
        return {Got::Ok, {}, {}};
    }
    return {Got::Transient, lastErr, "network"};
}

// A rename can fail for a moment on Windows while a scanner holds a file.
bool renameDir(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    for (int a = 0; a < 20; ++a) {
        fs::rename(from, to, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    err = "cannot move " + from.string() + " to " + to.string() + ": " + ec.message();
    return false;
}

}  // namespace

void ModelFetchJob::run() {
    // 0. A cancelled fetch of the same folder finishes stopping first.
    if (after_) {
        while (after_->snapshot().state == State::Running && !cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        after_.reset();
    }
    if (cancelled()) { finish(State::Cancelled, {}, "cancelled", "aborted"); return; }

    // 1. The first source whose listing has the folder.
    std::vector<FileEntry> files;
    const ModelSource* src = nullptr;
    std::string netErr;
    for (const ModelSource& s : sources_) {
        files.clear();
        std::string err;
        const Got g = listTree(this, s, name_, files, err, 0);
        if (g == Got::Cancelled) { finish(State::Cancelled, {}, "cancelled", "aborted"); return; }
        if (g == Got::Ok && !files.empty()) { src = &s; break; }
        if (g == Got::Transient || g == Got::Fatal) {
            if (!netErr.empty()) netErr += "; ";
            netErr += s.label + ": " + err;
        }
    }
    if (!src) {
        if (!netErr.empty()) {
            finish(State::Failed, {}, "could not reach the model sources (" + netErr + ")", "network");
        } else {
            std::string labels;
            for (const auto& s : sources_) labels += (labels.empty() ? "" : ", ") + s.label;
            finish(State::Failed, {}, name_ + " is in none of the model sources (" +
                   (labels.empty() ? std::string("none configured") : labels) + ")", "not-found");
        }
        return;
    }
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    total_.store(total);
    files_.store(static_cast<int>(files.size()));
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_ = src->label;
    }
    if (listOnly_) { finish(State::Done, {}, {}, {}); return; }

    // 2. Every file into the staging folder, verified.
    const fs::path stage = modelFetchStagingDir(root_, name_);
    const fs::path dest = (fs::path(root_) / fs::path(name_)).lexically_normal().make_preferred();
    std::error_code ec;
    fs::create_directories(stage, ec);
    if (ec) { finish(State::Failed, {}, "cannot create " + stage.string() + ": " + ec.message(), "io"); return; }
    const std::string prefix = name_ + "/";
    for (const FileEntry& f : files) {
        std::string rel = f.path.compare(0, prefix.size(), prefix) == 0 ? f.path.substr(prefix.size()) : f.path;
        const fs::path target = (stage / fs::path(rel)).lexically_normal();
        // A listing naming a path outside the folder is not one to trust.
        const std::string ts = target.string(), ss = stage.string();
        if (rel.empty() || ts.size() <= ss.size() || ts.compare(0, ss.size(), ss) != 0) {
            finish(State::Failed, {}, "the listing names an unsafe path: " + f.path, "integrity");
            return;
        }
        setFile(rel);
        // A file finished by an earlier fetch is checked again, not trusted.
        if (fs::exists(target, ec) && fs::file_size(target, ec) == f.size && !ec) {
            Hasher h(f);
            if (hashExisting(target, f.size, h, this) && h.matches(f)) { addDone(f.size); continue; }
            if (cancelled()) { finish(State::Cancelled, {}, "cancelled", "aborted"); return; }
            fs::remove(target, ec);
        }
        const FileResult r = fetchFile(this, *src, f, target);
        if (r.got == Got::Cancelled) { finish(State::Cancelled, {}, "cancelled", "aborted"); return; }
        if (r.got != Got::Ok) { finish(State::Failed, {}, r.err, r.code.empty() ? "network" : r.code); return; }
    }

    // 3. The folder into place in one rename.
    fs::create_directories(dest.parent_path(), ec);
    if (fs::is_directory(dest, ec)) {
        // Installed meanwhile (another bro): keep that one.
        fs::remove_all(stage, ec);
        finish(State::Done, dest.string(), {}, {});
        return;
    }
    std::string err;
    if (!renameDir(stage, dest, err)) { finish(State::Failed, {}, err, "io"); return; }
    // The staging folders left empty (remove() refuses one that is not).
    const fs::path stagingRoot = (fs::path(root_) / ".bro-fetch").lexically_normal();
    for (fs::path p = stage.parent_path(); !p.empty() && p.string().size() >= stagingRoot.string().size();
         p = p.parent_path()) {
        if (!fs::remove(p, ec) || ec) break;
    }
    finish(State::Done, dest.string(), {}, {});
}

#endif  // BRO_HAVE_CURL

} // namespace bro::util
