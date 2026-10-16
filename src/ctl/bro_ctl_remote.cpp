// bro-ctl --remote: bro_ctl_remote.h.
#include "ctl/bro_ctl_remote.h"

#include "platform/control_protocol.h"

#include <broremote/connect.h>
#include <broremote/text_client.h>

#include <brolink/loop.h>
#include <brolink/paths.h>
#include <brolink/wire.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace broctl {

namespace {

namespace control = bro::platform::control;
using Clock = std::chrono::steady_clock;

// brolink application of the agents' endpoints: apart from the engines'
// "bro-control", so a plain bro-ctl never mistakes an agent for a local bro.
constexpr std::string_view kAgentApp = "bro-ctl-remote";

broremote::ConnectTarget connectTarget(const RemoteTarget& t) {
    broremote::ConnectTarget c;
    c.ssh_host = t.host;
    c.socket = t.socket;
    c.socket_given = t.socketGiven;
    if (!t.sshCommand.empty()) {
        c.ssh_command = t.sshCommand;
        c.command_given = true;
    }
    return c;
}

std::string clientName() {
    return "bro-ctl agent (pid " + std::to_string(brolink::current_pid()) + ")";
}

std::filesystem::path agentFile(const RemoteTarget& t, const char* ext) {
    std::string dir = brolink::runtime_dir(kAgentApp);
    if (dir.empty()) dir = std::filesystem::temp_directory_path().string();
    return std::filesystem::path(dir) / (agentEndpoint(t) + ext);
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The agent: one text-only session to the host, served locally as a control
// endpoint. Requests from local clients go out with ids of the session's;
// replies come back on the TextClient's reader thread and are written by the
// loop thread.
class Agent final : public brolink::LoopHandler {
public:
    Agent(std::unique_ptr<broremote::TextClient> text, const RemoteTarget& t) : text_(std::move(text)), target_(t) {}

    int run(const std::string& address, std::string& err) {
        loop_ = brolink::EventLoop::create(*this);
        bool inUse = false;
        if (!loop_->listen(address, err, inUse)) {
            if (inUse) err = "another agent already serves " + address;
            return 2;
        }
        text_->on_closed([this](const std::string& why) {
            {
                std::lock_guard<std::mutex> lk(m_);
                ended_ = why;
            }
            loop_->wake();
        });
        lastUse_ = Clock::now();
        for (;;) {
            std::vector<std::pair<brolink::ConnId, std::string>> out;
            bool quit = false;
            std::string ended;
            {
                std::lock_guard<std::mutex> lk(m_);
                out.swap(replies_);
                quit = quit_;
                ended = ended_;
                if (!out.empty()) lastUse_ = Clock::now();
            }
            for (auto& [conn, msg] : out)
                if (conns_.count(conn)) loop_->write(conn, msg);
            if (!ended.empty()) {
                std::fprintf(stderr, "bro-ctl agent: the session ended: %s\n", ended.c_str());
                break;
            }
            if (quit) {
                // Let the reply to "@agent stop" go out first.
                for (int i = 0; i < 20 && anyPending(); ++i) loop_->run_once(10);
                break;
            }
            const bool idle = conns_.empty() && inFlight_.load() == 0 &&
                              Clock::now() - lastUse_ > std::chrono::seconds(target_.idleSeconds);
            if (idle) break;
            loop_->run_once(1000);
        }
        loop_->close_listener();
        // The session goes while the loop still exists: requests it still
        // had open are answered (and dropped) through it.
        text_->close();
        text_.reset();
        return 0;
    }

    void on_accept(brolink::ConnId id) override {
        conns_.emplace(id, brolink::wire::MessageSplitter(control::kMaxMessage));
        std::lock_guard<std::mutex> lk(m_);
        lastUse_ = Clock::now();
    }

    void on_data(brolink::ConnId id, const char* data, size_t n) override {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        it->second.feed(data, n);
        brolink::wire::MessageSplitter::Message m;
        while (it->second.next(m)) {
            uint64_t rid = 0, outArg = 0;
            std::vector<std::string> argv;
            if (m.type != control::kRequest || !control::decodeRequest(m.payload, rid, argv, outArg)) {
                loop_->write(id, control::encodeReply(rid, false, "bad request"));
                continue;
            }
            if (!argv.empty() && argv[0] == "@agent") {
                local(id, rid, argv);
                continue;
            }
            ++requests_;
            ++inFlight_;
            text_->request(std::move(argv), static_cast<size_t>(outArg), [this, id, rid](broremote::TextReply r) {
                --inFlight_;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    replies_.emplace_back(id, control::encodeReply(rid, r.ok, r.payload, r.file));
                }
                loop_->wake();
            });
        }
        if (it->second.error()) loop_->close(id, false);
    }

    void on_closed(brolink::ConnId id) override { conns_.erase(id); }

private:
    bool anyPending() {
        for (auto& [id, s] : conns_)
            if (loop_->pending_output(id)) return true;
        return false;
    }

    // Requests addressed to the agent itself (a first word of "@agent").
    void local(brolink::ConnId id, uint64_t rid, const std::vector<std::string>& argv) {
        const std::string what = argv.size() > 1 ? argv[1] : "status";
        if (what == "stop") {
            loop_->write(id, control::encodeReply(rid, true, "stopped the agent for " + target_.host));
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            return;
        }
        if (what == "status") {
            const auto up = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
            std::string s = "{\"host\":\"" + target_.host + "\",\"socket\":\"" + target_.socket +
                            "\",\"pid\":" + std::to_string(brolink::current_pid()) +
                            ",\"connected\":" + (text_->connected() ? "true" : "false") +
                            ",\"server\":\"" + text_->welcome().name + "\",\"protocol\":\"" +
                            std::to_string(text_->welcome().major) + "." + std::to_string(text_->welcome().minor) +
                            "\",\"uptimeSeconds\":" + std::to_string(up) + ",\"requests\":" +
                            std::to_string(requests_) + ",\"idleSeconds\":" + std::to_string(target_.idleSeconds) +
                            "}";
            loop_->write(id, control::encodeReply(rid, true, s));
            return;
        }
        loop_->write(id, control::encodeReply(rid, false, "@agent status | stop"));
    }

    std::unique_ptr<broremote::TextClient> text_;
    RemoteTarget target_;
    std::unique_ptr<brolink::EventLoop> loop_;
    std::map<brolink::ConnId, brolink::wire::MessageSplitter> conns_;  // loop thread
    std::atomic<int> inFlight_{0};
    uint64_t requests_ = 0;
    const Clock::time_point started_ = Clock::now();

    std::mutex m_;  // below
    std::vector<std::pair<brolink::ConnId, std::string>> replies_;
    std::string ended_;
    bool quit_ = false;
    Clock::time_point lastUse_;
};

class Direct final : public DirectLink {
public:
    explicit Direct(std::unique_ptr<broremote::TextClient> t) : text_(std::move(t)) {}
    bool call(const std::vector<std::string>& argv, size_t outArg, bool& ok, std::string& payload,
              std::string& file) override {
        broremote::TextReply r = text_->request_sync(argv, outArg);
        ok = r.ok;
        payload = std::move(r.payload);
        file = std::move(r.file);
        return ok || text_->connected();
    }

private:
    std::unique_ptr<broremote::TextClient> text_;
};

std::unique_ptr<broremote::TextClient> openSession(const RemoteTarget& t, std::string& err) {
    std::unique_ptr<brolink::Stream> s = broremote::open_stream(connectTarget(t), &err);
    if (!s) return nullptr;
    broremote::TextClientOptions o;
    o.name = clientName();
    o.timeout_ms = 20000;
    auto text = broremote::TextClient::open(std::move(s), o, &err);
    if (!text) err = t.host + ": " + err;
    return text;
}

}  // namespace

std::string agentEndpoint(const RemoteTarget& t) {
    std::string name = t.host;
    if (t.socketGiven && t.socket != "default") name += "_" + t.socket;
    for (char& c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok) c = '_';
    }
    if (name.size() > 64) name.resize(64);
    if (name.empty() || name == "." || name == "..") name = "_";
    return name;
}

std::unique_ptr<brolink::Stream> connectAgent(const RemoteTarget& t, std::string& err) {
    const std::string address = brolink::local_address(kAgentApp, agentEndpoint(t), &err);
    if (address.empty()) return nullptr;
    if (auto s = brolink::connect_local(address)) return s;

    // None answers: start one, and wait for it to serve (or say why not).
    const std::filesystem::path errFile = agentFile(t, ".err");
    std::error_code ec;
    std::filesystem::remove(errFile, ec);
    std::vector<std::string> argv = {brolink::current_executable(), "--remote", t.host, "--agent", "--idle",
                                     std::to_string(t.idleSeconds)};
    if (t.socketGiven) argv.insert(argv.end(), {"--remote-socket", t.socket});
    if (!t.sshCommand.empty()) argv.insert(argv.end(), {"--ssh-command", t.sshCommand});
    if (!brolink::spawn_detached(argv, &err)) {
        err = "cannot start the agent: " + err;
        return nullptr;
    }
    const auto until = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < until) {
        if (auto s = brolink::connect_local(address)) return s;
        if (std::filesystem::exists(errFile, ec)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the write may still be going
            err = readFile(errFile);
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            if (err.empty()) err = "the agent failed";
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    err = "the agent for " + t.host + " did not come up within 30 s";
    return nullptr;
}

int runAgent(const RemoteTarget& t) {
    const std::filesystem::path errFile = agentFile(t, ".err");
    auto fail = [&](const std::string& why) {
        std::ofstream(errFile, std::ios::binary) << why << "\n";
        return 2;
    };
    std::string err;
    const std::string address = brolink::local_address(kAgentApp, agentEndpoint(t), &err);
    if (address.empty()) return fail(err);
    auto text = openSession(t, err);
    if (!text) return fail(err);
    Agent agent(std::move(text), t);
    const int rc = agent.run(address, err);
    if (rc) return fail(err);
    return 0;
}

std::unique_ptr<DirectLink> DirectLink::open(const RemoteTarget& t, std::string& err) {
    auto text = openSession(t, err);
    if (!text) return nullptr;
    return std::make_unique<Direct>(std::move(text));
}

}  // namespace broctl
