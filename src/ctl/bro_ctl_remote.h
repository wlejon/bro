#pragma once

// bro-ctl --remote: the agent control commands of a remote session's engine
// (helm under `--drm --remote`), over broremote's text lane (protocol 1.6)
// instead of a shell on that machine (docs/agent-control.md, "Remote").
//
// Each bro-ctl run would otherwise pay an ssh login per command, so the
// connection is held by a local agent: a bro-ctl in the background
// (`--agent`, started by the first command, gone after it has been idle a
// while or when its session ends) that keeps one text-only broremote session
// open and serves this user a local control endpoint (brolink application
// "bro-ctl-remote", endpoint named for the host) speaking the same control
// protocol as an engine. A command is then a local connection to it.
// `--direct` skips the agent: one ssh per command.

#include <brolink/stream.h>

#include <memory>
#include <string>
#include <vector>

namespace broctl {

struct RemoteTarget {
    std::string host;                         // ssh destination
    std::string socket = "default";           // the broremote server's socket name there
    bool socketGiven = false;
    std::string sshCommand;                   // empty: broremote's default (`broremote proxy`)
    int idleSeconds = 1800;                   // the agent ends after this long with nothing asked
};

/// The agent's local endpoint name for `t` (a valid brolink name).
std::string agentEndpoint(const RemoteTarget& t);

/// A connection speaking the control protocol to the agent for `t`,
/// starting the agent when none answers. Null with `err` when it cannot.
std::unique_ptr<brolink::Stream> connectAgent(const RemoteTarget& t, std::string& err);

/// The agent itself (bro-ctl --remote HOST --agent): returns its exit status.
int runAgent(const RemoteTarget& t);

/// --direct: one command over a text-only session of its own.
class DirectLink {
public:
    static std::unique_ptr<DirectLink> open(const RemoteTarget& t, std::string& err);
    virtual ~DirectLink() = default;
    /// False when the connection broke (payload says why).
    virtual bool call(const std::vector<std::string>& argv, size_t outArg, bool& ok, std::string& payload,
                      std::string& file) = 0;
};

}  // namespace broctl
