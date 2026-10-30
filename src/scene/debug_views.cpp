#include "scene/debug_views.h"

#include <cctype>
#include <mutex>

namespace bro::scene::debug_views {

namespace {
std::mutex g_mutex;
std::vector<std::string> g_names;   // id - 1
} // namespace

int idOf(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_names.size(); ++i)
        if (g_names[i] == name) return static_cast<int>(i) + 1;
    g_names.push_back(name);
    return static_cast<int>(g_names.size());
}

int find(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_names.size(); ++i)
        if (g_names[i] == name) return static_cast<int>(i) + 1;
    return 0;
}

std::string nameOf(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return id >= 1 && id <= static_cast<int>(g_names.size()) ? g_names[id - 1] : std::string();
}

std::vector<std::string> names() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_names;
}

std::string expand(const std::string& source) {
    static const std::string kMacro = "SCENE_DEBUG(";
    std::string out;
    out.reserve(source.size());
    size_t at = 0;
    for (;;) {
        const size_t hit = source.find(kMacro, at);
        if (hit == std::string::npos) break;
        // A whole token: not the tail of a longer identifier.
        const bool joined = hit > 0 && (std::isalnum(static_cast<unsigned char>(source[hit - 1])) || source[hit - 1] == '_');
        const size_t close = source.find(')', hit);
        if (joined || close == std::string::npos) {
            out.append(source, at, hit + kMacro.size() - at);
            at = hit + kMacro.size();
            continue;
        }
        std::string name = source.substr(hit + kMacro.size(), close - hit - kMacro.size());
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        out.append(source, at, hit - at);
        out += "(sceneDebugView() == " + std::to_string(idOf(name)) + ")";
        at = close + 1;
    }
    out.append(source, at, std::string::npos);
    return out;
}

} // namespace bro::scene::debug_views
