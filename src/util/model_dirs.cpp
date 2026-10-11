#include "util/model_dirs.h"

#include "util/user_dirs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace bro::util {

namespace {

#ifdef _WIN32
constexpr char kListSep = ';';
#else
constexpr char kListSep = ':';
#endif

// One entry made absolute and normal, in the platform's separators. A
// trailing separator is dropped so "D:/models/" and "D:/models" are one entry.
std::string normalDir(const std::string& entry) {
    std::error_code ec;
    // Paths cross as the rest of bro's do (util/user_dirs.cpp, native_paths.cpp): path::string().
    fs::path p(entry);
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();
    std::string s = abs.make_preferred().string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        // Keep a root ("C:\", "/") whole.
        if (s.size() == 3 && s[1] == ':') break;
        s.pop_back();
    }
    return s;
}

// Windows paths compare without case; the others as spelled.
std::string sameKey(const std::string& s) {
#ifdef _WIN32
    std::string k = s;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return k;
#else
    return s;
#endif
}

void addList(const std::string& list, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(kListSep, start);
        if (end == std::string::npos) end = list.size();
        std::string entry = list.substr(start, end - start);
        // Whitespace around an entry is a typo, never part of a path that matters.
        const auto first = entry.find_first_not_of(" \t\r\n");
        const auto last = entry.find_last_not_of(" \t\r\n");
        entry = first == std::string::npos ? std::string() : entry.substr(first, last - first + 1);
        if (!entry.empty()) out.push_back(normalDir(entry));
        start = end + 1;
    }
}

}  // namespace

std::vector<std::string> modelDirs(const std::string& setting) {
    std::vector<std::string> all;
    if (const char* env = std::getenv("BRO_MODELS")) addList(env, all);
    addList(setting, all);
    all.push_back(normalDir((fs::path(userDataDir()) / "models").string()));

    std::vector<std::string> out;
    std::vector<std::string> keys;
    for (auto& d : all) {
        const std::string k = sameKey(d);
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        keys.push_back(k);
        out.push_back(std::move(d));
    }
    return out;
}

bool isModelName(const std::string& name) {
    if (name.empty()) return false;
    const fs::path p(name);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& seg : p) {
        if (seg == "..") return false;
    }
    return true;
}

std::string findModel(const std::string& name, const std::vector<std::string>& dirs) {
    if (!isModelName(name)) return {};
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path candidate = (fs::path(d) / fs::path(name)).lexically_normal();
        if (fs::is_directory(candidate, ec)) return fs::path(candidate).make_preferred().string();
    }
    return {};
}

} // namespace bro::util
