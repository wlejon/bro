#include "util/remote_asset.h"

#include "util/log.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef BRO_HAVE_CURL
#include <curl/curl.h>
#endif

namespace bro::util {
namespace {

/// Everything before the path: `https://host:port`. Empty when `u` is not a URL.
std::string urlOrigin(const std::string& u) {
    const size_t scheme = u.find("://");
    if (scheme == std::string::npos) return {};
    const size_t slash = u.find('/', scheme + 3);
    return slash == std::string::npos ? u : u.substr(0, slash);
}

/// The directory part of a URL — everything up to and including the last '/'
/// of the path, with query and fragment dropped (they belong to the document,
/// not to its neighbours).
std::string urlDirectory(const std::string& u) {
    std::string base = u.substr(0, u.find_first_of("?#"));
    const size_t scheme = base.find("://");
    const size_t from = (scheme == std::string::npos) ? 0 : scheme + 3;
    const size_t slash = base.find_last_of('/');
    if (slash == std::string::npos || slash < from) return base + "/";
    return base.substr(0, slash + 1);
}

/// RFC 3986 §5.2.4 remove_dot_segments, applied to the path of an absolute
/// URL. Done by hand rather than through std::filesystem because that would
/// rewrite '/' to '\' on Windows and eat the "//" after the scheme.
std::string removeDotSegments(const std::string& url) {
    const std::string origin = urlOrigin(url);
    if (origin.empty()) return url;
    std::string path = url.substr(origin.size());
    if (path.empty()) return url;

    std::string tail;                       // query/fragment, carried along
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) { tail = path.substr(cut); path = path.substr(0, cut); }

    std::vector<std::string> out;
    const bool trailingSlash = !path.empty() && path.back() == '/';
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') { ++i; continue; }
        const size_t end = path.find('/', i);
        std::string seg = path.substr(i, end == std::string::npos ? end : end - i);
        i = (end == std::string::npos) ? path.size() : end;
        if (seg == ".") continue;
        if (seg == "..") { if (!out.empty()) out.pop_back(); continue; }
        out.push_back(std::move(seg));
    }

    std::string rebuilt;
    for (const auto& seg : out) { rebuilt += '/'; rebuilt += seg; }
    if (rebuilt.empty()) rebuilt = "/";
    else if (trailingSlash) rebuilt += '/';
    return origin + rebuilt + tail;
}

std::string userDataDir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"))
        return std::string(appdata) + "/bro";
    if (const char* home = std::getenv("USERPROFILE"))
        return std::string(home) + "/AppData/Roaming/bro";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/Library/Application Support/bro";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return std::string(xdg) + "/bro";
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/.local/share/bro";
#endif
    return ".bro";
}

/// FNV-1a, enough to key a cache. Not a security boundary: the worst a
/// collision does is serve one CDN file in place of another, and the readable
/// suffix in the filename makes that visible rather than silent.
std::string hashUrl(const std::string& u) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : u) { h ^= c; h *= 1099511628211ull; }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

/// The last path segment, reduced to characters a filesystem is happy with, so
/// a human looking in the cache directory can tell what is in it.
std::string readableSuffix(const std::string& u) {
    std::string s = u.substr(0, u.find_first_of("?#"));
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.empty()) return "index";
    if (s.size() > 48) s = s.substr(s.size() - 48);
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '.' && c != '-' && c != '_')
            c = '_';
    }
    return s;
}

bool offlineOnly() {
    const char* v = std::getenv("BRO_OFFLINE");
    return v && *v && std::string(v) != "0";
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#ifdef BRO_HAVE_CURL
size_t writeToString(void* data, size_t size, size_t nmemb, void* userp) {
    const size_t n = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(data), n);
    return n;
}

/// One blocking GET. Empty on any failure; `status` reports what happened so
/// the caller can say something better than "it did not work".
std::string httpGet(const std::string& url, long& status, std::string& error) {
    CURL* easy = curl_easy_init();
    if (!easy) { error = "curl_easy_init failed"; return {}; }
    std::string body;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "bro");
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    const CURLcode rc = curl_easy_perform(easy);
    status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    else                error = curl_easy_strerror(rc);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK) return {};
    if (status < 200 || status >= 300) { error = "HTTP " + std::to_string(status); return {}; }
    return body;
}
#endif

} // namespace

bool hasUrlScheme(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) ||
                            (i > 0 && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                                       s[i] == '+' || s[i] == '-' || s[i] == '.'))))
        ++i;
    // A one-letter "scheme" is a Windows drive letter, so require two.
    return i > 1 && s.compare(i, 3, "://") == 0;
}

bool isHttpUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

std::string resolveUrl(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (hasUrlScheme(rel)) return rel;
    if (!hasUrlScheme(base)) return rel;

    if (rel.rfind("//", 0) == 0) {
        const size_t scheme = base.find("://");
        return base.substr(0, scheme + 1) + rel;
    }
    if (rel[0] == '/') return removeDotSegments(urlOrigin(base) + rel);
    if (rel[0] == '?' || rel[0] == '#')
        return base.substr(0, base.find_first_of("?#")) + rel;
    return removeDotSegments(urlDirectory(base) + rel);
}

bool remoteFetchAvailable() {
#ifdef BRO_HAVE_CURL
    return true;
#else
    return false;
#endif
}

std::string remoteCachePath(const std::string& url) {
    return userDataDir() + "/remote-cache/" + hashUrl(url) + "-" + readableSuffix(url);
}

std::string fetchRemoteCached(const std::string& url) {
    const std::string cached = remoteCachePath(url);

    std::error_code ec;
    if (std::filesystem::exists(cached, ec)) {
        std::string body = readFile(cached);
        if (!body.empty()) return body;
    }

    if (offlineOnly()) {
        LOG_ERROR("[remote] BRO_OFFLINE is set and '%s' is not cached", url.c_str());
        return {};
    }
#ifndef BRO_HAVE_CURL
    LOG_ERROR("[remote] no HTTP client compiled in; cannot fetch '%s'", url.c_str());
    return {};
#else
    long status = 0;
    std::string error;
    LOG_INFO("[remote] fetching %s", url.c_str());
    std::string body = httpGet(url, status, error);
    if (body.empty()) {
        LOG_ERROR("[remote] %s: %s", url.c_str(),
                  error.empty() ? "empty response" : error.c_str());
        return {};
    }

    std::filesystem::create_directories(
        std::filesystem::path(cached).parent_path(), ec);
    // Write through a temporary and rename, so a killed run cannot leave a
    // truncated file that every later run then happily serves from cache.
    const std::string tmp = cached + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    std::filesystem::rename(tmp, cached, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return body;
#endif
}

} // namespace bro::util
