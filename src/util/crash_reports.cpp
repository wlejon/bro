#include "util/crash_reports.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#ifdef BRO_HAVE_CURL
#include <curl/curl.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace bro::util {

namespace fs = std::filesystem;

namespace {

constexpr const char* kCrashesDir = "crashes";
constexpr const char* kSentSuffix = ".sent";
constexpr size_t kKeepSent = 5;
// A log is sent from its end: the crash is there, and a chatty run's start
// is not worth megabytes.
constexpr size_t kMaxLogBytes = 4u << 20;
constexpr uintmax_t kMaxDumpBytes = 64u << 20;

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string str(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string readTail(const fs::path& p, size_t maxBytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > static_cast<std::streamoff>(maxBytes) ? size - static_cast<std::streamoff>(maxBytes) : 0;
    in.seekg(from);
    std::string out(static_cast<size_t>(size - from), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

// The crash handler's banner: "=== <program> crash (<how>) ===".
bool holdsCrashReport(const std::string& log) {
    for (size_t at = log.find("=== "); at != std::string::npos; at = log.find("=== ", at + 4)) {
        const size_t eol = log.find('\n', at);
        const std::string line = log.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.find(" crash (") != std::string::npos) return true;
    }
    return false;
}

// A running bro still writes this log (it holds it as bro_main's
// redirectLogToFile opened it: deny-write on Windows, an flock elsewhere), so
// its missing clean-exit line means nothing yet.
bool logInUse(const fs::path& p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
#else
    const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool busy = flock(fd, LOCK_SH | LOCK_NB) != 0;
    close(fd);
    return busy;
#endif
}

std::string kindOf(const std::string& log) { return holdsCrashReport(log) ? "crash" : "unclean-exit"; }

std::string timeStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string hostName() {
#ifdef _WIN32
    if (const char* n = std::getenv("COMPUTERNAME")) return n;
    return {};
#else
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) == 0) return buf;
    return {};
#endif
}

#ifdef BRO_HAVE_CURL
size_t discard(char*, size_t size, size_t n, void*) { return size * n; }

// One file of a report, POSTed as its body; true when the receiver took it.
bool post(const CrashReportInfo& info, const std::string& report, const char* file, const std::string& kind,
          const std::string& body) {
    CURL* easy = curl_easy_init();
    if (!easy) return false;
    const std::string url = info.url + (info.url.find('?') == std::string::npos ? "?" : "&") + "report=" + report +
                            "&file=" + file;
    struct curl_slist* headers = nullptr;
    auto add = [&](const char* name, const std::string& value) {
        // Header values stay on one line.
        std::string v = value;
        std::replace(v.begin(), v.end(), '\r', ' ');
        std::replace(v.begin(), v.end(), '\n', ' ');
        headers = curl_slist_append(headers, (std::string(name) + ": " + v).c_str());
    };
    add("Content-Type", std::string(file) == "log" ? "text/plain; charset=utf-8" : "application/octet-stream");
    add("X-Bro-Report-Kind", kind);
    add("X-Bro-App", info.appId);
    add("X-Bro-App-Name", info.appName);
    add("X-Bro-App-Version", info.appVersion);
    add("X-Bro-Engine", info.engineVersion);
    add("X-Bro-Host", hostName());
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "bro-crash-report");
    const CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return rc == CURLE_OK && status >= 200 && status < 300;
}
#endif

void pruneSent(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> sent;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = str(e.path().filename());
        if (name.size() > 8 && name.ends_with(std::string(".log") + kSentSuffix)) sent.push_back(e.path());
    }
    if (sent.size() <= kKeepSent) return;
    // Names carry their time, so name order is age order.
    std::sort(sent.begin(), sent.end());
    for (size_t i = 0; i + kKeepSent < sent.size(); ++i) {
        fs::remove(sent[i], ec);
        fs::path dmp = sent[i];
        dmp.replace_filename(str(sent[i].filename()).substr(0, str(sent[i].filename()).size() - 9) + ".dmp" + kSentSuffix);
        fs::remove(dmp, ec);
    }
}

} // namespace

int sendPendingCrashReports(const std::string& logDir, const CrashReportInfo& info) {
    int sent = 0;
#ifdef BRO_HAVE_CURL
    if (info.url.empty()) return 0;
    const fs::path dir = u8(logDir) / kCrashesDir;
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".log") logs.push_back(e.path());
    std::sort(logs.begin(), logs.end());
    for (const fs::path& log : logs) {
        const std::string report = str(log.stem());
        const std::string text = readTail(log, kMaxLogBytes);
        const std::string kind = kindOf(text);
        if (!post(info, report, "log", kind, text)) return sent;   // unreachable: try again next launch
        fs::path dmp = log;
        dmp.replace_extension(".dmp");
        if (fs::exists(dmp, ec)) {
            const uintmax_t size = fs::file_size(dmp, ec);
            if (!ec && size <= kMaxDumpBytes) {
                std::ifstream in(dmp, std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!post(info, report, "dmp", kind, bytes)) return sent;
            }
            fs::rename(dmp, fs::path(dmp).concat(kSentSuffix), ec);
        }
        fs::rename(log, fs::path(log).concat(kSentSuffix), ec);
        std::fprintf(stderr, "crash report %s sent to %s\n", report.c_str(), info.url.c_str());
        ++sent;
    }
    pruneSent(dir);
#else
    (void)logDir;
    (void)info;
#endif
    return sent;
}

std::string keepCrashedLog(const std::string& logFile, const std::string& dumpFile) {
    std::error_code ec;
    const fs::path log = u8(logFile);
    const fs::path dump = dumpFile.empty() ? fs::path() : u8(dumpFile);
    if (logInUse(log)) return {};
    const bool hasDump = !dump.empty() && fs::exists(dump, ec);
    std::string kept;
    if (fs::exists(log, ec) && fs::file_size(log, ec) > 0) {
        const std::string text = readTail(log, kMaxLogBytes);
        const bool crashed = holdsCrashReport(text);
        const bool clean = text.find(kCleanExitLine) != std::string::npos;
        if (crashed || !clean) {
            const fs::path dir = log.parent_path() / kCrashesDir;
            fs::create_directories(dir, ec);
            // Two in one second (a crash loop) each keep a name of their own.
            const std::string base = str(log.stem()) + "-" + timeStamp();
            std::string stem = base;
            for (int n = 2; fs::exists(dir / (stem + ".log"), ec) || fs::exists(dir / (stem + ".log.sent"), ec); ++n)
                stem = base + "-" + std::to_string(n);
            const fs::path to = dir / (stem + ".log");
            fs::copy_file(log, to, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                kept = str(to);
                if (hasDump) fs::rename(dump, dir / (stem + ".dmp"), ec);
            }
        }
    }
    if (!dump.empty()) fs::remove(dump, ec);   // a dump nobody kept is stale
    return kept;
}

void noteCleanExit() {
    std::fprintf(stderr, "%s\n", kCleanExitLine);
    std::fflush(stderr);
}

void sendCrashReports(const std::string& logDir, const CrashReportInfo& info) {
    if (info.url.empty() || logDir.empty()) return;
    std::thread([logDir, info] { sendPendingCrashReports(logDir, info); }).detach();
}

} // namespace bro::util
