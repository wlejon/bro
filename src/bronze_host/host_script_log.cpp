#include "bronze_host/host_script_log.h"

#include "embed/embed.h"
#include "eval/eval.h"
#include "util/log.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

namespace {

std::mutex g_scriptLogMu;
std::string g_scriptLogPath;

constexpr const char* kWrapConsoleSource = R"JS((function () {
  var c = globalThis.console;
  if (!c || c.__broScriptLogWrapped) return;
  c.__broScriptLogWrapped = true;
  var sink = globalThis.__bro_write_script_log;
  if (typeof sink !== 'function') return;
  function fmtArg(a) {
    if (typeof a === 'string') return a;
    if (a !== null && typeof a === 'object') {
      if (a instanceof Error || (typeof a.message === 'string' && typeof a.name === 'string')) {
        return typeof a.stack === 'string' && a.stack ? a.stack : (a.name || 'Error') + ': ' + a.message;
      }
      try {
        var s = JSON.stringify(a);
        if (s !== undefined) return s;
      } catch (e) {}
    }
    return String(a);
  }
  var levels = ['log', 'info', 'warn', 'error', 'debug'];
  for (var i = 0; i < levels.length; i++) {
    (function (lvl) {
      var orig = c[lvl];
      if (typeof orig !== 'function') return;
      c[lvl] = function () {
        var parts = [];
        for (var j = 0; j < arguments.length; j++) {
          parts.push(fmtArg(arguments[j]));
        }
        try { sink(lvl, parts.join(' ')); } catch (e) {}
        return orig.apply(c, arguments);
      };
    })(levels[i]);
  }
})();)JS";

std::filesystem::path u8Path(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

}  // namespace

void setScriptLogPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_scriptLogMu);
    if (path.empty()) {
        g_scriptLogPath.clear();
        return;
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(u8Path(path), ec);
    if (ec) p = u8Path(path);
    const std::u8string u8 = p.u8string();
    g_scriptLogPath.assign(u8.begin(), u8.end());

    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("script-log: failed to open '%s' for writing", g_scriptLogPath.c_str());
    }
}

bool hasScriptLogPath() {
    std::lock_guard<std::mutex> lk(g_scriptLogMu);
    return !g_scriptLogPath.empty();
}

void writeScriptLogLine(const std::string& level, const std::string& line) {
    std::lock_guard<std::mutex> lk(g_scriptLogMu);
    if (g_scriptLogPath.empty()) return;
    const std::filesystem::path p = u8Path(g_scriptLogPath);
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    if (!level.empty()) {
        out << '[' << level << "] ";
    }
    out << line << '\n';
    out.flush();
}

void installScriptLogSink() {
    if (!hasScriptLogPath()) return;
    if (!ev::currentRealm()) return;

    ev::GlobalValue consoleG = ev::globalValue("console");
    if (!consoleG.found || !ev::isObject(consoleG.value)) return;
    {
        ev::Persistent consoleObj(consoleG.value);
        Value wrapped = ev::getProperty(consoleObj.get(), "__broScriptLogWrapped");
        if (ev::isBool(wrapped) && ev::toBool(wrapped)) return;
    }

    const ev::Persistent sinkFn(ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            const std::string level = a.size() > 0 ? ev::toUtf8(a[0]) : "log";
            const std::string line = a.size() > 1 ? ev::toUtf8(a[1]) : std::string();
            writeScriptLogLine(level, line);
            return ev::undefined();
        },
        2, "__bro_write_script_log"));
    ev::registerGlobal("__bro_write_script_log", sinkFn.get());

    bronze::eval::EvalOptions opts;
    opts.filename = "<script-log-sink>";
    opts.hostGlobals = ev::hostGlobalNames();
    auto compiled = bronze::eval::compileScript(kWrapConsoleSource, opts);
    auto res = bronze::eval::runCompiledScript(std::move(compiled), opts);
    if (res.thrown) {
        ev::Persistent errRoot(res.value);
        LOG_WARN("script-log: failed to install console sink: %s", ev::toUtf8(errRoot.get()).c_str());
    }
}

}  // namespace bro::bronze_host
