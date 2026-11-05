// util/crash_reports: which logs a launch keeps as crash reports, and the
// send. BRO_CRASH_TEST_URL=<receiver>/crash also sends one to a live
// receiver (scripts/crash-receiver.js) and checks it was accepted; without
// it the send is checked against a port nothing listens on, which must
// leave the report waiting.

#include "util/crash_handler.h"
#include "util/crash_reports.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace bro::util;

static int g_failures = 0;
#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (cond) std::printf("  ok    %s\n", what);                              \
        else { std::printf("  FAIL  %s (%s:%d)\n", what, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

static fs::path utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

static void write(const fs::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

static size_t count(const fs::path& dir, const std::string& suffix) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) ++n;
    }
    return n;
}

int main(int argc, char** argv) {
    // The child the crash check runs: the handler armed as a windowed bro
    // arms it, a minidump path set, then abort().
    if (argc == 3 && std::string(argv[1]) == "--crash") {
        installCrashHandler("bro", CrashMode::Exit);
        setCrashDumpPath(argv[2]);
        std::abort();
    }

    const fs::path root = fs::temp_directory_path() / ("bro_crash_reports_test_" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path log = root / "org.example.app.log";
    const fs::path dmp = root / "org.example.app.dmp";
    const fs::path crashes = root / "crashes";
    const std::string logS = log.string(), dmpS = dmp.string();

    std::printf("keeping\n");
    CHECK(keepCrashedLog(logS, dmpS).empty(), "no log: nothing kept");

    write(log, "started\nworking\n" + std::string(kCleanExitLine) + "\n");
    write(dmp, "stale");
    CHECK(keepCrashedLog(logS, dmpS).empty(), "a cleanly closed log is not kept");
    CHECK(!fs::exists(dmp), "a dump nobody kept is deleted");

    write(log, "");
    CHECK(keepCrashedLog(logS, dmpS).empty(), "an empty log is not kept");

    write(log, "started\nworking\n");
    const std::string unclean = keepCrashedLog(logS, dmpS);
    CHECK(!unclean.empty() && fs::exists(utf8(unclean)), "a log without the clean-exit line is kept");

    write(log, "started\n\n=== bro crash (unhandled) ===\n  code    0xC0000005\n=== end crash ===\n" +
                   std::string(kCleanExitLine) + "\n");
    write(dmp, "MDMP");
    const std::string crashed = keepCrashedLog(logS, dmpS);
    CHECK(!crashed.empty(), "a log holding a crash report is kept, clean-exit line or not");
    fs::path keptDump = utf8(crashed);
    keptDump.replace_extension(".dmp");
    CHECK(fs::exists(keptDump) && !fs::exists(dmp), "its minidump goes with it");
    CHECK(count(crashes, ".log") == 2, "both kept in crashes/, each under its own name");

    // A log a running bro still writes (as bro_main opens it) is not a crash.
    write(log, "still running\n");
#ifdef _WIN32
    const int held = _wsopen(log.wstring().c_str(), _O_WRONLY | _O_APPEND, _SH_DENYWR, _S_IREAD | _S_IWRITE);
#else
    const int held = open(log.c_str(), O_WRONLY | O_APPEND);
    if (held >= 0) flock(held, LOCK_EX | LOCK_NB);
#endif
    CHECK(held >= 0, "the log held open for writing");
    const size_t before = count(crashes, ".log");
    CHECK(keepCrashedLog(logS, dmpS).empty() && count(crashes, ".log") == before, "a log in use is left alone");
#ifdef _WIN32
    _close(held);
#else
    close(held);
#endif

    std::printf("sending\n");
    CrashReportInfo info{"http://127.0.0.1:9/crash", "org.example.app", "Example", "1.0", "test"};
    const size_t waiting = count(crashes, ".log");
    CHECK(sendPendingCrashReports(root.string(), info) == 0 && count(crashes, ".log") == waiting,
          "an unreachable receiver leaves the reports waiting");
    info.url.clear();
    CHECK(sendPendingCrashReports(root.string(), info) == 0, "no URL sends nothing");

    if (const char* url = std::getenv("BRO_CRASH_TEST_URL")) {
        info.url = url;
        const int sent = sendPendingCrashReports(root.string(), info);
        CHECK(sent == static_cast<int>(waiting), "the live receiver accepted every report");
        CHECK(count(crashes, ".log") == 0 && count(crashes, ".log.sent") == waiting, "sent reports are marked sent");
    } else {
        std::printf("  (BRO_CRASH_TEST_URL unset: no live send)\n");
    }

    std::printf("crashing\n");
    {
        // The child's stderr is the log its crash report goes to; then the
        // report a launch would keep from it.
        const fs::path childLog = root / "child.log";
        const fs::path childDump = root / "child.dmp";
        const std::string cmd = "\"\"" + fs::absolute(argv[0]).string() + "\" --crash \"" + childDump.string() +
                                "\" 2> \"" + childLog.string() + "\"\"";
#ifndef _WIN32
        const std::string run = cmd.substr(1, cmd.size() - 2);
#else
        const std::string& run = cmd;
#endif
        const int rc = std::system(run.c_str());
        CHECK(rc != 0, "the child crashed");
        std::ifstream in(childLog, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("=== bro crash (SIGABRT) ===") != std::string::npos, "its log holds the crash report");
#ifdef _WIN32
        std::ifstream dumpIn(childDump, std::ios::binary);
        char magic[4] = {0};
        dumpIn.read(magic, 4);
        CHECK(std::string(magic, 4) == "MDMP", "it wrote a minidump");
        CHECK(text.find("minidump ") != std::string::npos, "the report names the minidump");
#endif
        CHECK(!keepCrashedLog(childLog.string(), childDump.string()).empty(), "a launch keeps that log");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
