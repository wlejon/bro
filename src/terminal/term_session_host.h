#pragma once
// TermSession's TerminalHost (private to src/terminal): what the program
// tells the embedder, turned into TermEvents. Every callback runs with the
// session's lock held, on the parser thread or on the main thread inside an
// input call or feed(); it only queues.

#include "terminal/term_session.h"

namespace bro::terminal {

class TermSession::Host final : public bropty::TerminalHost {
public:
    explicit Host(TermSession& s) : s_(s) {}

    void bell() override;
    void title_changed(std::string_view title) override;
    void cwd_changed(std::string_view uri) override;
    void clipboard_write(std::string_view selection, std::string_view data) override;
    std::optional<std::string> clipboard_read(std::string_view selection) override;
    bool clipboard_read_async(uint64_t request, std::string_view selection) override;
    void notification_ex(const bropty::Notification& n) override;
    void progress(int state, int value) override;
    void semantic_mark(char kind, std::string_view params) override;
    void pointer_shape_changed(std::string_view name) override;

private:
    TermSession& s_;
};

} // namespace bro::terminal
