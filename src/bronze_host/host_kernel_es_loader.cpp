#include "bronze_host/host_kernel_es_loader.h"

#include "eval/eval.h"

#include <brokernel/ast.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

bool readTextFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return false;
    out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

std::string jsStringLiteral(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

bool isValidJsExportIdent(const std::string& name) {
    if (name.empty()) return false;
    auto isStart = [](unsigned char c) { return std::isalpha(c) || c == '_' || c == '$'; };
    auto isCont = [&](unsigned char c) { return isStart(c) || std::isdigit(c); };
    if (!isStart(static_cast<unsigned char>(name[0]))) return false;
    for (size_t i = 1; i < name.size(); ++i) {
        if (!isCont(static_cast<unsigned char>(name[i]))) return false;
    }
    static const std::unordered_set<std::string_view> kReserved = {
        "break", "case", "catch", "class", "const", "continue", "debugger", "default",
        "delete", "do", "else", "enum", "export", "extends", "false", "finally",
        "for", "function", "if", "import", "in", "instanceof", "new", "null",
        "return", "super", "switch", "this", "throw", "true", "try", "typeof",
        "var", "void", "while", "with", "yield", "let", "await", "static",
        "implements", "interface", "package", "private", "protected", "public",
        "undefined", "__bkSrc", "__bkMod", "__source"
    };
    return !kReserved.contains(name);
}

void collectLocalDeps(const std::filesystem::path& entryPath,
                      const std::string& entrySource,
                      std::vector<std::pair<std::string, std::string>>& outModules,
                      std::vector<std::string>& outFilesRead) {
    namespace fs = std::filesystem;
    std::set<std::string> visitedPaths;
    std::unordered_set<std::string> seenModuleNames;

    auto canonStr = [](const fs::path& p) -> std::string {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(p, ec);
        auto u8 = (ec ? p : c).generic_u8string();
        return std::string(u8.begin(), u8.end());
    };

    visitedPaths.insert(canonStr(entryPath));

    struct QueueItem {
        fs::path path;
        std::string source;
    };
    std::vector<QueueItem> queue;
    queue.push_back({entryPath, entrySource});

    while (!queue.empty()) {
        QueueItem cur = std::move(queue.back());
        queue.pop_back();

        brokernel::DiagnosticBag diags;
        auto ast = brokernel::parse_source(cur.source, cur.path.string(), diags);
        if (!ast) continue;

        const fs::path curDir = cur.path.parent_path();
        for (const brokernel::ImportDecl& imp : ast->imports) {
            fs::path depPath;
            std::string modName;
            if (!imp.file_path.empty()) {
                depPath = curDir / std::filesystem::path(imp.file_path);
                modName = imp.alias.empty() ? depPath.stem().string() : imp.alias;
            } else if (imp.module_path.size() == 1 && imp.module_path[0] != "std") {
                modName = imp.module_path[0];
                depPath = curDir / (modName + ".bk");
            } else {
                continue;
            }

            std::error_code ec;
            if (!fs::is_regular_file(depPath, ec)) continue;
            const std::string key = canonStr(depPath);
            if (!visitedPaths.insert(key).second) continue;

            std::string depSrc;
            if (!readTextFile(depPath, depSrc)) continue;
            outFilesRead.push_back(depPath.string());
            if (!modName.empty() && seenModuleNames.insert(modName).second) {
                outModules.emplace_back(modName, depSrc);
            }
            queue.push_back({depPath, std::move(depSrc)});
        }
    }
}

bronze::modules::CustomModuleResult loadKernelEsModule(const std::filesystem::path& resolvedPath) {
    if (resolvedPath.extension() != ".bk") return {};

    bronze::modules::CustomModuleResult res;
    res.handled = true;

    std::string source;
    if (!readTextFile(resolvedPath, source)) {
        res.error = "cannot read .bk module '" + resolvedPath.string() + "'";
        return res;
    }
    res.extraFilesRead.push_back(resolvedPath.string());

    std::vector<std::pair<std::string, std::string>> deps;
    collectLocalDeps(resolvedPath, source, deps, res.extraFilesRead);

    brokernel::DiagnosticBag diags;
    auto ast = brokernel::parse_source(source, resolvedPath.string(), diags);

    std::ostringstream js;
    for (const auto& [mname, msrc] : deps) {
        js << "bro.kernel.registerModule(" << jsStringLiteral(mname) << ", " << jsStringLiteral(msrc) << ");\n";
    }
    const std::string stem = resolvedPath.stem().string();
    const auto u8Path = resolvedPath.generic_u8string();
    const std::string fileStr(u8Path.begin(), u8Path.end());

    js << "const __bkSrc = " << jsStringLiteral(source) << ";\n";
    if (isValidJsExportIdent(stem) || (!stem.empty() && stem.find('/') == std::string::npos)) {
        js << "bro.kernel.registerModule(" << jsStringLiteral(stem) << ", __bkSrc);\n";
    }
    js << "const __bkMod = bro.kernel.compile(__bkSrc, " << jsStringLiteral(fileStr) << ");\n";
    js << "export const __source = __bkSrc;\n";

    if (ast) {
        std::unordered_set<std::string> exported;
        for (const brokernel::FnDecl& fn : ast->functions) {
            if (!isValidJsExportIdent(fn.name)) continue;
            if (!exported.insert(fn.name).second) continue;
            if (fn.is_entry_kernel()) {
                js << "export const " << fn.name << " = __bkMod.kernel(" << jsStringLiteral(fn.name) << ");\n";
            } else if (fn.is_pub) {
                js << "export const " << fn.name << " = __bkMod.fn(" << jsStringLiteral(fn.name) << ");\n";
            }
        }
    }
    js << "export default __bkMod;\n";
    res.jsSource = js.str();
    return res;
}

}  // namespace

void configureKernelEsLoader(bronze::eval::EvalOptions& opts) {
    if (std::find(opts.extraModuleExtensions.begin(), opts.extraModuleExtensions.end(), ".bk") ==
        opts.extraModuleExtensions.end()) {
        opts.extraModuleExtensions.push_back(".bk");
    }
    opts.customModuleLoader = loadKernelEsModule;
}

}  // namespace bro::bronze_host
