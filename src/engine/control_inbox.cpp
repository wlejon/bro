// The agent inbox's control commands (docs/agent-inbox.md): `agents`, `ask`
// and `inbox <sub>`, over AgentInbox (agent_inbox.h). Free text and files
// travel as options (`--text=`, `--files=` with a platform/inbox_bundle.h
// bundle), never as positional arguments, so a text that starts with "--"
// or a bundle of any bytes stays whole. Files coming back (a request's for
// the serving agent, a reply's for the asker) are written to the command's
// output-path argument, which a remote client names as its outArg and so
// gets the bytes of in the reply.

#include "engine/agent_inbox.h"
#include "engine/control.h"

#include <filesystem>
#include <fstream>

namespace bro::engine {

namespace {

std::filesystem::path fsPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Replies on `call`, writing the files bundle to `out` first when it is
// given (always, an empty bundle included, so the client finds a file).
AgentInbox::Reply replyTo(const ControlCallPtr& call, std::string out) {
    return [call, out = std::move(out)](bool ok, std::string payload, std::string bundle) {
        if (ok && !out.empty()) {
            if (bundle.empty()) bundle = platform::inbox::encode({});
            std::ofstream o(fsPath(out), std::ios::binary | std::ios::trunc);
            o.write(bundle.data(), static_cast<std::streamsize>(bundle.size()));
            if (!o) return call->fail("cannot write the files to " + out);
        }
        if (ok) call->ok(std::move(payload));
        else call->fail(std::move(payload));
    };
}

AgentInbox::Sender senderOf(const ControlCallPtr& call) {
    AgentInbox::Sender s;
    s.agent = call->option("from");
    s.origin = call->option("origin");
    s.via = call->origin();
    return s;
}

double secondsMs(const ControlCallPtr& call, const char* name, double fallbackSeconds) {
    return call->number(name, fallbackSeconds) * 1000.0;
}

void cmdInbox(AgentInbox& inbox, const ControlCallPtr& call) {
    const std::string sub = call->arg(0);
    auto usage = [&](const char* u) { call->fail(std::string("usage: inbox ") + u); };
    if (sub == "register") {
        if (call->arg(1).empty()) return usage("register <name> [--about=TEXT] [--replace]");
        return inbox.registerAgent(call->arg(1), call->option("about"), call->flag("replace"), call->origin(),
                                   replyTo(call, {}));
    }
    if (sub == "next") {
        if (call->arg(1).empty()) return usage("next <token> [out] [--timeout=ms]");
        return inbox.next(call->arg(1), call->number("timeout", AgentInbox::kMaxPollMs), replyTo(call, call->arg(2)));
    }
    if (sub == "progress") {
        if (call->arg(2).empty()) return usage("progress <token> <id> --text=TEXT");
        return inbox.progress(call->arg(1), call->arg(2), call->option("text"), replyTo(call, {}));
    }
    if (sub == "reply") {
        if (call->arg(2).empty()) return usage("reply <token> <id> [--text=TEXT] [--files=BUNDLE] [--failed]");
        return inbox.answer(call->arg(1), call->arg(2), call->option("text"), call->option("files"),
                            call->flag("failed"), replyTo(call, {}));
    }
    if (sub == "unregister") {
        if (call->arg(1).empty()) return usage("unregister <token>");
        return inbox.unregister(call->arg(1), replyTo(call, {}));
    }
    if (sub == "status") {
        if (call->arg(1).empty()) return usage("status <id> [--since=N] [--wait=SECS]");
        return inbox.status(call->arg(1), static_cast<size_t>(std::max(0.0, call->number("since", 0))),
                            secondsMs(call, "wait", 0), replyTo(call, {}));
    }
    if (sub == "result") {
        if (call->arg(1).empty()) return usage("result <id> [out] [--wait=SECS]");
        return inbox.result(call->arg(1), secondsMs(call, "wait", 0), replyTo(call, call->arg(2)));
    }
    if (sub == "cancel") {
        if (call->arg(1).empty()) return usage("cancel <id> [--from=NAME]");
        return inbox.cancel(call->arg(1), senderOf(call), replyTo(call, {}));
    }
    if (sub == "events") {
        return inbox.events(static_cast<uint64_t>(std::max(0.0, call->number("since", 0))),
                            secondsMs(call, "wait", 0), replyTo(call, {}));
    }
    if (sub == "log") {
        return inbox.log(static_cast<size_t>(std::max(0.0, call->number("limit", 50))), call->option("id"),
                         replyTo(call, {}));
    }
    call->fail("usage: inbox register|next|progress|reply|unregister|status|result|cancel|events|log ... "
               "(docs/agent-inbox.md)");
}

}  // namespace

void registerInboxCommands(ControlServer& server) {
    AgentInbox& inbox = server.inbox();
    server.registerCommand("agents", "[--json]  the agents registered in this session's inbox",
                           [&inbox](const ControlCallPtr& call) {
                               inbox.agents(call->flag("json"), replyTo(call, {}));
                           });
    server.registerCommand(
        "ask",
        "<agent> [out] --text=TEXT [--files=BUNDLE] [--from=NAME] [--origin=USER@HOST] [--wait=SECS]  "
        "send an agent a request: its id now, or with --wait its reply (files to out)",
        [&inbox](const ControlCallPtr& call) {
            const std::string to = call->arg(0);
            if (to.empty()) return call->fail("usage: ask <agent> [out] --text=TEXT [--wait=SECS]");
            inbox.ask(to, call->option("text"), call->option("files"), senderOf(call), secondsMs(call, "wait", 0),
                      replyTo(call, call->arg(1)));
        });
    server.registerCommand("inbox",
                           "register|next|progress|reply|unregister|status|result|cancel|events|log ...  "
                           "the agent inbox (docs/agent-inbox.md)",
                           [&inbox](const ControlCallPtr& call) { cmdInbox(inbox, call); });
}

}  // namespace bro::engine
