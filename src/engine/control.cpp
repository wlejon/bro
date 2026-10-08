#include "engine/control.h"

#include "engine/engine.h"
#include "platform/control_socket.h"
#include "util/log.h"
#include "util/time.h"

#include <cstdlib>
#include <exception>

namespace bro::engine {

// ---------------------------------------------------------------------------
// ControlCall
// ---------------------------------------------------------------------------

std::vector<std::string> ControlCall::positional() const {
    std::vector<std::string> out;
    for (size_t i = 1; i < argv_.size(); ++i) {
        const std::string& a = argv_[i];
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') continue;
        out.push_back(a);
    }
    return out;
}

std::string ControlCall::arg(size_t i, const std::string& fallback) const {
    auto p = positional();
    return i < p.size() ? p[i] : fallback;
}

std::string ControlCall::option(const std::string& name, const std::string& fallback) const {
    const std::string key = "--" + name;
    for (size_t i = 1; i < argv_.size(); ++i) {
        const std::string& a = argv_[i];
        if (a == key) return "true";
        if (a.size() > key.size() && a.compare(0, key.size(), key) == 0 && a[key.size()] == '=')
            return a.substr(key.size() + 1);
    }
    return fallback;
}

bool ControlCall::flag(const std::string& name) const {
    std::string v = option(name);
    return !v.empty() && v != "false" && v != "0" && v != "no";
}

double ControlCall::number(const std::string& name, double fallback) const {
    std::string v = option(name);
    if (v.empty()) return fallback;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    return (end && end != v.c_str()) ? d : fallback;
}

// ---------------------------------------------------------------------------
// ControlServer
// ---------------------------------------------------------------------------

ControlServer::ControlServer(Engine& engine)
    : engine_(engine), socket_(std::make_unique<platform::ControlSocket>()) {
    registerCommand("help", "[command]  list the commands, or one command's usage",
                    [this](const ControlCallPtr& call) {
                        const std::string which = call->arg(0);
                        if (which.empty()) return call->ok(helpText());
                        auto it = commands_.find(which);
                        if (it == commands_.end()) return call->fail("no command '" + which + "'");
                        call->ok(which + " " + it->second.usage + "\n");
                    });
}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(const std::string& name, std::string* why) {
    if (!socket_->start(name, why)) return false;
    LOG_INFO("Engine: agent control on %s (bro-ctl help)", socket_->path().c_str());
    return true;
}

void ControlServer::stop() { socket_->stop(); }

double ControlServer::clockMs() const {
    if (engine_.displayMode() == DisplayMode::Headless) return engine_.virtualTime();
    return util::currentTimeMs();
}
bool ControlServer::running() const { return socket_->running(); }
std::string ControlServer::socketPath() const { return socket_->path(); }
int ControlServer::pollFd() const { return socket_->running() ? socket_->readyFd() : -1; }

void ControlServer::registerCommand(const std::string& name, const std::string& usage, Handler handler) {
    commands_[name] = Command{usage, std::move(handler)};
}

std::string ControlServer::helpText() const {
    std::string out;
    for (const auto& [name, cmd] : commands_) out += name + " " + cmd.usage + "\n";
    return out;
}

void ControlServer::dispatch(std::vector<std::string> argv, std::function<void(bool, std::string)> sink) {
    if (argv.empty()) {
        sink(false, "empty command (try: help)");
        return;
    }
    auto it = commands_.find(argv[0]);
    if (it == commands_.end()) {
        sink(false, "no command '" + argv[0] + "' (try: help)");
        return;
    }
    auto call = std::make_shared<ControlCall>(std::move(argv), std::move(sink));
    try {
        it->second.handler(call);
    } catch (const std::exception& e) {
        call->fail(std::string("command threw: ") + e.what());
    }
}

void ControlServer::pump() {
    if (socket_->running()) {
        for (auto& req : socket_->take()) {
            const uint64_t conn = req.conn;
            auto reply = [this, conn](bool ok, std::string payload) { socket_->reply(conn, ok, std::move(payload)); };
            if (!req.parseError.empty()) {
                reply(false, req.parseError);
                continue;
            }
            dispatch(std::move(req.argv), reply);
        }
    }
    if (tickers_.empty()) return;
    // A ticker may add tickers (a command built from others): run the ones
    // present at the start, keep the unfinished and the new.
    std::vector<Ticker> running;
    running.swap(tickers_);
    std::vector<Ticker> keep;
    for (auto& t : running)
        if (!t()) keep.push_back(std::move(t));
    for (auto& t : tickers_) keep.push_back(std::move(t));
    tickers_.swap(keep);
}

}  // namespace bro::engine
