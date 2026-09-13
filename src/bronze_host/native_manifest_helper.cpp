#include "bronze_host/native_manifest_helper.h"
#include "bronze_host/eval.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace bro::bronze_host {

namespace {

bool hasJsonFiles(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().extension() == ".json") {
            return true;
        }
    }
    return false;
}

std::string checkManifestCandidate(const std::filesystem::path& cand) {
    std::error_code ec;
    if (hasJsonFiles(cand)) {
        return std::filesystem::absolute(cand, ec).string();
    }
    if (std::filesystem::is_regular_file(cand, ec) && cand.extension() == ".json") {
        return std::filesystem::absolute(cand, ec).string();
    }
    return {};
}

} // namespace

std::string getNativeManifestDir() {
    // 1. Check environment variables
    const char* env = std::getenv("BRO_NATIVE_MANIFEST");
    if (!env || !*env) env = std::getenv("BRONZE_NATIVE_MANIFEST");
    if (env && *env) {
        if (std::strcmp(env, "none") == 0 || std::strcmp(env, "off") == 0 || std::strcmp(env, "0") == 0) {
            return {};
        }
        std::string found = checkManifestCandidate(std::filesystem::path(env));
        if (!found.empty()) return found;
    }

    // 2. Probe candidate paths
    std::vector<std::filesystem::path> candidates;

    if (const char* root = std::getenv("BRO_PROJECT_ROOT")) {
        std::filesystem::path r(root);
        candidates.push_back(r / "third_party/brosurface/out/c_abi/manifest");
        candidates.push_back(r / "../brosurface/out/c_abi/manifest");
        candidates.push_back(r / "out/c_abi/manifest");
    }

    const auto exeDir = getExecutableDirectory();
    candidates.push_back(exeDir / "../third_party/brosurface/out/c_abi/manifest");
    candidates.push_back(exeDir / "../../third_party/brosurface/out/c_abi/manifest");
    candidates.push_back(exeDir / "../../../third_party/brosurface/out/c_abi/manifest");
    candidates.push_back(exeDir / "../brosurface/out/c_abi/manifest");
    candidates.push_back(exeDir / "../../brosurface/out/c_abi/manifest");
    candidates.push_back(exeDir / "third_party/brosurface/out/c_abi/manifest");

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "third_party/brosurface/out/c_abi/manifest");
        candidates.push_back(cwd / "../third_party/brosurface/out/c_abi/manifest");
        candidates.push_back(cwd / "../brosurface/out/c_abi/manifest");
        candidates.push_back(cwd / "../../brosurface/out/c_abi/manifest");
        candidates.push_back(cwd / "brosurface/out/c_abi/manifest");
    }

    for (const auto& cand : candidates) {
        std::string found = checkManifestCandidate(cand);
        if (!found.empty()) return found;
    }

    return {};
}

std::string getNativeLibPath() {
    // 1. Check environment variables
    const char* env = std::getenv("BRO_NATIVE_LIB");
    if (!env || !*env) env = std::getenv("BRONZE_NATIVE_LIB");
    if (env && *env) {
        if (std::strcmp(env, "none") == 0 || std::strcmp(env, "off") == 0 || std::strcmp(env, "0") == 0) {
            return {};
        }
        std::error_code ec;
        std::filesystem::path p(env);
        if (std::filesystem::is_regular_file(p, ec)) {
            return std::filesystem::absolute(p, ec).string();
        }
    }

    // 2. Candidate library file names
    const std::vector<std::string> names = {
#ifdef _WIN32
        "bro_c_abi.dll",
        "bro_c_abi.lib",
        "libbro_c_abi.dll",
        "libbro_c_abi.lib",
#elif defined(__APPLE__)
        "libbro_c_abi.dylib",
        "libbro_c_abi.so",
#else
        "libbro_c_abi.so",
#endif
        "libbro_c_abi.so",
        "bro_c_abi.dll",
        "libbro_c_abi.dylib",
        "bro_c_abi.lib",
        "libbro_c_abi.lib",
        "libbro_c_abi.a"
    };

    // 3. Candidate base directories
    std::vector<std::filesystem::path> baseDirs;

    if (const char* rtEnv = std::getenv("BRONZE_SHARED_RT_LIB")) {
        std::filesystem::path rt(rtEnv);
        auto p = rt.parent_path();
        baseDirs.push_back(p);
        baseDirs.push_back(p / "Release");
        baseDirs.push_back(p / "Debug");
        auto parent = p.parent_path();
        if (!parent.empty()) {
            baseDirs.push_back(parent);
            baseDirs.push_back(parent / "Release");
            baseDirs.push_back(parent / "Debug");
            baseDirs.push_back(parent / "src/c_abi");
            baseDirs.push_back(parent / "src/c_abi/Release");
            baseDirs.push_back(parent / "src/c_abi/Debug");
        }
    }

    const auto exeDir = getExecutableDirectory();
    baseDirs.push_back(exeDir);
    baseDirs.push_back(exeDir / "Release");
    baseDirs.push_back(exeDir / "Debug");
    baseDirs.push_back(exeDir / "src/c_abi");
    baseDirs.push_back(exeDir / "src/c_abi/Release");
    baseDirs.push_back(exeDir / "src/c_abi/Debug");

    auto exeParent = exeDir.parent_path();
    if (!exeParent.empty()) {
        baseDirs.push_back(exeParent);
        baseDirs.push_back(exeParent / "Release");
        baseDirs.push_back(exeParent / "Debug");
        baseDirs.push_back(exeParent / "build");
        baseDirs.push_back(exeParent / "build/Release");
        baseDirs.push_back(exeParent / "build/Debug");
        baseDirs.push_back(exeParent / "build-release");
        baseDirs.push_back(exeParent / "build/src/c_abi");
        baseDirs.push_back(exeParent / "build/src/c_abi/Release");
        baseDirs.push_back(exeParent / "build/src/c_abi/Debug");
    }

    if (const char* rootEnv = std::getenv("BRO_PROJECT_ROOT")) {
        std::filesystem::path root(rootEnv);
        baseDirs.push_back(root / "build");
        baseDirs.push_back(root / "build/Release");
        baseDirs.push_back(root / "build/Debug");
        baseDirs.push_back(root / "build-release");
        baseDirs.push_back(root / "build/src/c_abi");
        baseDirs.push_back(root / "build/src/c_abi/Release");
        baseDirs.push_back(root / "build/src/c_abi/Debug");
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        baseDirs.push_back(cwd);
        baseDirs.push_back(cwd / "build");
        baseDirs.push_back(cwd / "build/Release");
        baseDirs.push_back(cwd / "build/Debug");
        baseDirs.push_back(cwd / "build-release");
        baseDirs.push_back(cwd / "build/src/c_abi");
        baseDirs.push_back(cwd / "build/src/c_abi/Release");
        baseDirs.push_back(cwd / "build/src/c_abi/Debug");
    }

    for (const auto& dir : baseDirs) {
        for (const auto& name : names) {
            auto cand = dir / name;
            if (std::filesystem::is_regular_file(cand, ec)) {
                return std::filesystem::absolute(cand, ec).string();
            }
        }
    }

    return {};
}

} // namespace bro::bronze_host
