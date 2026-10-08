#include "engine/source_preflight.h"

#include "engine/app_loader.h"
#include "util/asset_mounts.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

bool readText(const fs::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;
}

// The text with comments blanked out (replaced by spaces, so offsets and
// line structure stay), string and template literal contents kept. Regex
// literals are not recognised; one containing `//` would cut its line short,
// which at worst hides an import and so errs towards "consistent".
std::string stripComments(std::string_view src) {
    std::string out(src);
    size_t i = 0;
    const size_t n = out.size();
    while (i < n) {
        const char c = out[i];
        if (c == '\'' || c == '"' || c == '`') {
            const char q = c;
            ++i;
            while (i < n && out[i] != q) {
                if (out[i] == '\\') ++i;
                else if (q != '`' && out[i] == '\n') break;  // unterminated
                ++i;
            }
            ++i;
        } else if (c == '/' && i + 1 < n && out[i + 1] == '/') {
            while (i < n && out[i] != '\n') out[i++] = ' ';
        } else if (c == '/' && i + 1 < n && out[i + 1] == '*') {
            out[i] = out[i + 1] = ' ';
            i += 2;
            while (i < n && !(out[i] == '*' && i + 1 < n && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i < n) { out[i] = ' '; if (i + 1 < n) out[i + 1] = ' '; i += 2; }
        } else {
            ++i;
        }
    }
    return out;
}

struct Cursor {
    std::string_view s;
    size_t i = 0;
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool at(char c) const { return i < s.size() && s[i] == c; }
    bool word(std::string_view w) {
        if (s.compare(i, w.size(), w) != 0) return false;
        const size_t e = i + w.size();
        if (e < s.size() && isIdentChar(s[e])) return false;
        i = e;
        return true;
    }
    std::string ident() {
        const size_t b = i;
        while (i < s.size() && isIdentChar(s[i])) ++i;
        return std::string(s.substr(b, i - b));
    }
    bool quoted(std::string& out) {
        if (!(at('\'') || at('"'))) return false;
        const char q = s[i++];
        const size_t b = i;
        while (i < s.size() && s[i] != q && s[i] != '\n') ++i;
        if (!at(q)) return false;
        out = std::string(s.substr(b, i - b));
        ++i;
        return true;
    }
    // `{ a, b as c, default as d }` — calls f(left, right) per item, right
    // empty when there is no `as`. Leaves the cursor after `}`.
    template <class F> bool braceList(F f) {
        if (!at('{')) return false;
        ++i;
        while (true) {
            ws();
            if (at('}')) { ++i; return true; }
            if (i >= s.size()) return false;
            std::string left;
            if (!quoted(left)) left = ident();
            if (left.empty()) return false;
            ws();
            std::string right;
            if (word("as")) {
                ws();
                if (!quoted(right)) right = ident();
                ws();
            }
            f(left, right);
            if (at(',')) ++i;
        }
    }
};

struct ImportEdge {
    std::string specifier;
    std::vector<std::string> names;   // names the importer needs the target to export
};

struct ModuleScan {
    std::vector<ImportEdge> imports;
    std::set<std::string> exports;
    bool exportsUncertain = false;    // `export *`, destructuring, multi-declarators
};

ModuleScan scanModule(std::string_view rawSrc) {
    ModuleScan out;
    const std::string src = stripComments(rawSrc);
    Cursor c{src};
    auto skipString = [&](size_t& i) {
        const char q = src[i++];
        while (i < src.size() && src[i] != q) {
            if (src[i] == '\\') ++i;
            else if (q != '`' && src[i] == '\n') break;
            ++i;
        }
        ++i;
    };
    for (size_t i = 0; i < src.size();) {
        const char ch = src[i];
        if (ch == '\'' || ch == '"' || ch == '`') { skipString(i); continue; }
        const bool boundary = i == 0 || (!isIdentChar(src[i - 1]) && src[i - 1] != '.');
        if (!boundary || !(ch == 'i' || ch == 'e')) { ++i; continue; }
        c.i = i;
        if (c.word("import")) {
            c.ws();
            if (c.at('(') || c.at('.')) { i = c.i; continue; }   // import() / import.meta
            ImportEdge edge;
            if (c.quoted(edge.specifier)) { out.imports.push_back(edge); i = c.i; continue; }
            // Clause: default, `* as ns`, `{ ... }`, or default followed by one of those.
            bool ok = true;
            if (c.at('*')) {
                ++c.i; c.ws();
                if (!c.word("as")) ok = false;
                c.ws(); c.ident();
            } else if (c.at('{')) {
                ok = c.braceList([&](const std::string& l, const std::string&) { edge.names.push_back(l); });
            } else {
                const std::string def = c.ident();
                if (def.empty()) ok = false;
                else {
                    edge.names.push_back("default");
                    c.ws();
                    if (c.at(',')) {
                        ++c.i; c.ws();
                        if (c.at('*')) { ++c.i; c.ws(); c.word("as"); c.ws(); c.ident(); }
                        else ok = c.braceList([&](const std::string& l, const std::string&) { edge.names.push_back(l); });
                    }
                }
            }
            c.ws();
            if (ok && c.word("from")) {
                c.ws();
                if (c.quoted(edge.specifier)) out.imports.push_back(std::move(edge));
            }
            i = std::max(c.i, i + 1);
            continue;
        }
        c.i = i;
        if (c.word("export")) {
            c.ws();
            if (c.at('*')) {
                ++c.i; c.ws();
                if (c.word("as")) { c.ws(); out.exports.insert(c.ident()); c.ws(); }
                else out.exportsUncertain = true;
                if (c.word("from")) {
                    c.ws();
                    ImportEdge edge;
                    if (c.quoted(edge.specifier)) out.imports.push_back(std::move(edge));
                }
            } else if (c.at('{')) {
                std::vector<std::pair<std::string, std::string>> items;
                c.braceList([&](const std::string& l, const std::string& r) { items.emplace_back(l, r); });
                for (auto& [l, r] : items) out.exports.insert(r.empty() ? l : r);
                c.ws();
                if (c.word("from")) {
                    c.ws();
                    ImportEdge edge;
                    for (auto& [l, r] : items) edge.names.push_back(l);
                    if (c.quoted(edge.specifier)) out.imports.push_back(std::move(edge));
                }
            } else if (c.word("default")) {
                out.exports.insert("default");
            } else {
                c.word("async");
                c.ws();
                if (c.word("function")) {
                    c.ws();
                    if (c.at('*')) { ++c.i; c.ws(); }
                    out.exports.insert(c.ident());
                } else if (c.word("class")) {
                    c.ws();
                    out.exports.insert(c.ident());
                } else if (c.word("const") || c.word("let") || c.word("var")) {
                    c.ws();
                    if (c.at('{') || c.at('[')) {
                        out.exportsUncertain = true;
                    } else {
                        out.exports.insert(c.ident());
                        // A second declarator (`export const a = 1, b = 2`)
                        // is a comma at bracket depth 0 before the statement
                        // ends; without semicolons the end is not knowable
                        // here, so any such comma makes the list uncertain.
                        int depth = 0;
                        for (size_t k = c.i; k < src.size(); ++k) {
                            const char d = src[k];
                            if (d == '\'' || d == '"' || d == '`') { skipString(k); --k; continue; }
                            if (d == '(' || d == '[' || d == '{') ++depth;
                            else if (d == ')' || d == ']' || d == '}') { if (--depth < 0) break; }
                            else if (d == ';' && depth == 0) break;
                            else if (d == ',' && depth == 0) { out.exportsUncertain = true; break; }
                        }
                    }
                } else {
                    out.exportsUncertain = true;   // a form this scan does not know
                }
            }
            i = std::max(c.i, i + 1);
            continue;
        }
        ++i;
    }
    return out;
}

// A path the manifest could not map to the disk: `/app/x.js` with no `/app`
// mount, say, left as written for the runtime's own module roots to resolve.
// Not this check's to judge.
bool unresolvedRootPath(const std::string& p) {
    if (p.empty() || (p[0] != '/' && p[0] != '\\')) return false;
    const fs::path path(p);
    auto it = path.begin();
    if (it == path.end()) return false;
    ++it;   // past the root
    if (it == path.end()) return false;
    std::error_code ec;
    return !fs::exists(path.root_path() / *it, ec);
}

bool isScriptFile(const fs::path& p) {
    const std::string ext = p.extension().string();
    return ext == ".js" || ext == ".mjs" || ext == ".cjs";
}

class Checker {
public:
    Checker(fs::path appDir, const util::AssetMounts* mounts)
        : appDir_(std::move(appDir)), mounts_(mounts) {}

    // Empty when `spec` (imported from a file in `fromDir`) names a file that
    // exists, or is not a specifier this check follows.
    std::string resolve(const fs::path& fromDir, const std::string& spec, fs::path& out) const {
        if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
            out = (fromDir / spec).lexically_normal();
        } else if (spec.rfind("/", 0) == 0 && spec.rfind("//", 0) != 0) {
            const std::string m = mounts_ ? mounts_->resolve(spec) : std::string();
            if (m.empty()) return {};   // resolved by module roots this check does not model
            out = fs::path(m).lexically_normal();
        } else {
            return {};   // bare specifier or URL
        }
        if (isFile(out)) return {};
        for (const char* suffix : {".js", ".mjs", "/index.js"}) {
            fs::path alt = fs::path(out.string() + suffix);
            if (isFile(alt)) { out = alt; return {}; }
        }
        return "missing";
    }

    const ModuleScan* scan(const fs::path& file) {
        auto it = scans_.find(file.string());
        if (it != scans_.end()) return &it->second;
        std::string text;
        if (!readText(file, text)) return nullptr;
        return &scans_.emplace(file.string(), scanModule(text)).first->second;
    }

    std::string rel(const fs::path& p) const {
        std::error_code ec;
        fs::path r = fs::relative(p, appDir_, ec);
        if (ec || r.empty() || r.native().rfind("..", 0) == 0) return p.generic_string();
        return r.generic_string();
    }

    // Walks the static import graph from `file`.
    std::string walk(const fs::path& file, const std::string& inlineSource = {}) {
        const ModuleScan* m = nullptr;
        ModuleScan inlineScan;
        if (!inlineSource.empty()) {
            inlineScan = scanModule(inlineSource);
            m = &inlineScan;
        } else {
            if (!visited_.insert(file.string()).second) return {};
            m = scan(file);
            if (!m) return rel(file) + " cannot be read";
        }
        const fs::path dir = inlineSource.empty() ? file.parent_path() : file;
        const std::string who = inlineSource.empty() ? rel(file) : "an inline module script";
        for (const auto& edge : m->imports) {
            fs::path target;
            if (!resolve(dir, edge.specifier, target).empty())
                return who + " imports '" + edge.specifier + "', which does not exist";
            if (target.empty() || !isScriptFile(target)) continue;
            const ModuleScan* t = scan(target);
            if (!t) return who + " imports " + rel(target) + ", which cannot be read";
            if (!t->exportsUncertain) {
                for (const auto& name : edge.names) {
                    if (!t->exports.count(name))
                        return who + " imports " + name + " from " + rel(target) +
                               ", which does not export it";
                }
            }
            if (std::string why = walk(target); !why.empty()) return why;
        }
        return {};
    }

private:
    fs::path appDir_;
    const util::AssetMounts* mounts_;
    std::unordered_map<std::string, ModuleScan> scans_;
    std::set<std::string> visited_;
};

} // namespace

std::string checkAppSourceConsistency(const std::string& appDir,
                                      const util::AssetMounts* mounts) {
    std::error_code ec;
    const fs::path root = fs::absolute(appDir, ec);
    const fs::path index = root / "index.html";
    if (isFile(index)) {
        std::string html;
        if (!readText(index, html) || html.empty()) return "index.html is empty";
    } else if (!isFile(root / "main.js") && !isFile(root / "server.js")) {
        return "index.html does not exist";
    }

    const AppManifest manifest = AppLoader::loadApp(root.string(), mounts);
    // Stylesheets are not checked: a missing one costs the page its styling,
    // not its life, and cold start tolerates it the same way.
    Checker checker(root, mounts);
    for (const auto& script : manifest.scripts) {
        if (script.isInline()) {
            if (script.isModule) {
                if (std::string why = checker.walk(root, script.code); !why.empty()) return why;
            }
            continue;
        }
        if (script.path.find("://") != std::string::npos || unresolvedRootPath(script.path)) continue;
        if (!isFile(script.path)) return "script " + checker.rel(script.path) + " does not exist";
        if (script.isModule) {
            if (std::string why = checker.walk(fs::path(script.path).lexically_normal()); !why.empty())
                return why;
        }
    }
    return {};
}

} // namespace bro::engine
