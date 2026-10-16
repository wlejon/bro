#pragma once

// bro-ctl's agent inbox commands (docs/agent-inbox.md), built over the
// engine's `ask` and `inbox ...` commands:
//
//   bro-ctl ask NAME TEXT... [--file PATH]... [--from NAME] [--wait SECS]
//                            [--async] [--save DIR] [--json]
//   bro-ctl inbox serve NAME [--about TEXT] [--exec CMD] [--replace] [--dir DIR]
//   bro-ctl inbox result ID [--wait SECS] [--save DIR] [--json]
//   bro-ctl inbox status ID [--follow]
//   bro-ctl inbox log [--limit N] [--id ID] [--json]
//
// Files go to the engine bundled in a `--files=` argument and come back
// through an output-path argument (the request's outArg when the engine is
// remote), so every one of these works over --remote as it does locally.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broctl {

/// One connection to an engine (local, or a remote session's through the agent).
class Caller {
public:
    virtual ~Caller() = default;
    /// One command and its reply; false when the connection broke.
    virtual bool call(const std::vector<std::string>& argv, size_t outArg, bool& ok, std::string& payload,
                      std::string& file) = 0;
    /// The engine is on another machine: an output file comes back in the
    /// reply (outArg) instead of being written where the engine runs.
    virtual bool remote() const = 0;
};

/// Opens a fresh connection (serve keeps two: one polls, one answers).
using CallerFactory = std::function<std::unique_ptr<Caller>(std::string& err)>;

/// Whether bro-ctl handles `args` here rather than passing them through.
bool isInboxCommand(const std::vector<std::string>& args);

/// Runs it: the exit status (0 ok, 1 failed, 2 no connection, 3 still running).
int runInbox(const std::vector<std::string>& args, Caller& c, const CallerFactory& open,
             const std::string& runtimeDir);

}  // namespace broctl
