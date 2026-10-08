#include "platform/control_socket.h"

#include "util/log.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace bro::platform {

// ---------------------------------------------------------------------------
// The request line: a JSON array of strings.
// ---------------------------------------------------------------------------

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

bool parseJsonStringArray(const std::string& s, std::vector<std::string>& out, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    size_t i = 0;
    auto ws = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    };
    auto hex4 = [&](uint32_t& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    };
    out.clear();
    ws();
    if (i >= s.size() || s[i] != '[') return fail("expected a JSON array of strings");
    ++i;
    ws();
    if (i < s.size() && s[i] == ']') return true;
    for (;;) {
        ws();
        if (i >= s.size()) return fail("unterminated array");
        std::string item;
        if (s[i] == '"') {
            ++i;
            for (;;) {
                if (i >= s.size()) return fail("unterminated string");
                char c = s[i++];
                if (c == '"') break;
                if (c != '\\') {
                    item += c;
                    continue;
                }
                if (i >= s.size()) return fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': item += '"'; break;
                    case '\\': item += '\\'; break;
                    case '/': item += '/'; break;
                    case 'b': item += '\b'; break;
                    case 'f': item += '\f'; break;
                    case 'n': item += '\n'; break;
                    case 'r': item += '\r'; break;
                    case 't': item += '\t'; break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!hex4(cp)) return fail("bad \\u escape");
                        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            uint32_t lo = 0;
                            if (!hex4(lo)) return fail("bad \\u escape");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(item, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
            }
        } else {
            // A bare scalar (number, true, false, null): its text.
            while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != ' ') item += s[i++];
            if (item.empty()) return fail("expected a string");
        }
        out.push_back(std::move(item));
        ws();
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == ']') return true;
        return fail("expected , or ]");
    }
}

std::string ControlSocket::socketDir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string base = (xdg && *xdg) ? xdg : std::filesystem::temp_directory_path().string();
    return (std::filesystem::path(base) / "bro-control").string();
}

#if !defined(_WIN32)

namespace {

void setNonBlockCloexec(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

}  // namespace

struct ControlSocket::Impl {
    int listenFd = -1;
    int wakeRead = -1, wakeWrite = -1;
    int readyRead = -1, readyWrite = -1;  // readable while requests wait for take()
    std::string path;
    std::thread thread;
    std::atomic<bool> running{false};

    struct Conn {
        int fd = -1;
        std::string in;
        std::string out;
        int pending = 0;   // requests handed out, not yet replied to
        bool eof = false;  // the peer finished sending
    };
    std::map<uint64_t, Conn> conns;  // thread-owned
    uint64_t nextConn = 1;

    std::mutex mu;  // guards the two queues below
    std::vector<Request> requests;
    std::vector<std::pair<uint64_t, std::string>> replies;

    void wake() {
        if (wakeWrite >= 0) {
            char c = 1;
            (void)::write(wakeWrite, &c, 1);
        }
    }

    bool peerIsUs(int fd) {
#if defined(SO_PEERCRED)
        struct ucred cred {};
        socklen_t len = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
        return cred.uid == ::getuid();
#else
        uid_t uid = 0;
        gid_t gid = 0;
        if (getpeereid(fd, &uid, &gid) != 0) return false;
        return uid == ::getuid();
#endif
    }

    void loop() {
        std::vector<pollfd> fds;
        std::vector<uint64_t> ids;
        while (running.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lk(mu);
                for (auto& [id, text] : replies) {
                    auto it = conns.find(id);
                    if (it == conns.end()) continue;
                    it->second.out += text;
                    --it->second.pending;
                }
                replies.clear();
            }
            fds.clear();
            ids.clear();
            fds.push_back({listenFd, POLLIN, 0});
            fds.push_back({wakeRead, POLLIN, 0});
            for (auto& [id, c] : conns) {
                // A peer done sending, with nothing to write to it yet, is
                // left out: its hangup would wake the poll on every turn.
                if (c.eof && c.out.empty()) continue;
                short ev = c.eof ? 0 : POLLIN;
                if (!c.out.empty()) ev |= POLLOUT;
                fds.push_back({c.fd, ev, 0});
                ids.push_back(id);
            }
            int n = ::poll(fds.data(), fds.size(), 500);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (fds[1].revents & POLLIN) {
                char buf[64];
                while (::read(wakeRead, buf, sizeof(buf)) > 0) {
                }
            }
            if (fds[0].revents & POLLIN) {
                int cfd = ::accept(listenFd, nullptr, nullptr);
                if (cfd >= 0) {
                    setNonBlockCloexec(cfd);
                    if (!peerIsUs(cfd)) {
                        ::close(cfd);
                    } else {
                        conns[nextConn++] = Conn{cfd, {}, {}};
                    }
                }
            }
            for (size_t k = 0; k < ids.size(); ++k) {
                const pollfd& p = fds[k + 2];
                auto it = conns.find(ids[k]);
                if (it == conns.end()) continue;
                Conn& c = it->second;
                bool drop = false;
                if (p.revents & POLLOUT) {
                    ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                    if (w > 0) c.out.erase(0, static_cast<size_t>(w));
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) drop = true;
                }
                if (p.revents & POLLIN) {
                    char buf[4096];
                    ssize_t r = ::read(c.fd, buf, sizeof(buf));
                    if (r > 0) {
                        c.in.append(buf, static_cast<size_t>(r));
                        size_t nl;
                        while ((nl = c.in.find('\n')) != std::string::npos) {
                            std::string line = c.in.substr(0, nl);
                            c.in.erase(0, nl + 1);
                            if (line.empty() || line == "\r") continue;
                            Request req;
                            req.conn = ids[k];
                            if (!parseJsonStringArray(line, req.argv, &req.parseError) && req.parseError.empty())
                                req.parseError = "bad request";
                            ++c.pending;
                            std::lock_guard<std::mutex> lk(mu);
                            if (requests.empty() && readyWrite >= 0) {
                                char one = 1;
                                (void)::write(readyWrite, &one, 1);
                            }
                            requests.push_back(std::move(req));
                        }
                        if (c.in.size() > (1u << 20)) drop = true;  // a 1 MB line is not a command
                    } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        // The peer is done sending; it still gets the replies
                        // to what it asked.
                        c.eof = true;
                    }
                }
                if (p.revents & (POLLERR | POLLNVAL)) drop = true;
                if ((c.eof || (p.revents & POLLHUP)) && c.pending <= 0 && c.out.empty()) drop = true;
                if (drop) {
                    ::close(c.fd);
                    conns.erase(it);
                }
            }
        }
        for (auto& [id, c] : conns) ::close(c.fd);
        conns.clear();
    }
};

ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}
ControlSocket::~ControlSocket() { stop(); }

bool ControlSocket::running() const { return impl_->running.load(std::memory_order_acquire); }
const std::string& ControlSocket::path() const { return impl_->path; }

bool ControlSocket::start(const std::string& name, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (running()) return true;
    const std::string dir = socketDir();
    ::mkdir(dir.c_str(), 0700);
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return fail("cannot create " + dir);
    if (st.st_uid != ::getuid()) return fail(dir + " belongs to another user");
    ::chmod(dir.c_str(), 0700);

    const std::string path = dir + "/" + name + ".sock";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return fail("socket path too long: " + path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    // A live server already on this name keeps it; a dead one's file goes.
    {
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0) {
            if (::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                ::close(probe);
                return fail("another process is serving " + path);
            }
            ::close(probe);
        }
        ::unlink(path.c_str());
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fail(std::string("socket: ") + std::strerror(errno));
    setNonBlockCloexec(fd);
    mode_t old = ::umask(0177);
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::umask(old);
    if (rc != 0) {
        ::close(fd);
        return fail("bind " + path + ": " + std::strerror(errno));
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return fail(std::string("listen: ") + std::strerror(errno));
    }
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return fail("pipe failed");
    }
    int readyFds[2];
    if (::pipe(readyFds) != 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::close(fd);
        ::unlink(path.c_str());
        return fail("pipe failed");
    }
    for (int p : {pipeFds[0], pipeFds[1], readyFds[0], readyFds[1]}) setNonBlockCloexec(p);
    impl_->listenFd = fd;
    impl_->wakeRead = pipeFds[0];
    impl_->wakeWrite = pipeFds[1];
    impl_->readyRead = readyFds[0];
    impl_->readyWrite = readyFds[1];
    impl_->path = path;
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this] { impl_->loop(); });
    return true;
}

void ControlSocket::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->wake();
    if (impl_->thread.joinable()) impl_->thread.join();
    ::close(impl_->listenFd);
    ::close(impl_->wakeRead);
    ::close(impl_->wakeWrite);
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        ::close(impl_->readyRead);
        ::close(impl_->readyWrite);
        impl_->readyRead = impl_->readyWrite = -1;
    }
    impl_->listenFd = impl_->wakeRead = impl_->wakeWrite = -1;
    ::unlink(impl_->path.c_str());
}

std::vector<ControlSocket::Request> ControlSocket::take() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<Request> out;
    out.swap(impl_->requests);
    if (impl_->readyRead >= 0) {
        char buf[64];
        while (::read(impl_->readyRead, buf, sizeof(buf)) > 0) {}
    }
    return out;
}

int ControlSocket::readyFd() const { return impl_ ? impl_->readyRead : -1; }

void ControlSocket::reply(uint64_t conn, bool ok, std::string payload) {
    std::string text = (ok ? "ok " : "error ") + std::to_string(payload.size()) + "\n";
    text += payload;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->replies.emplace_back(conn, std::move(text));
    }
    impl_->wake();
}

#else  // _WIN32

struct ControlSocket::Impl {
    std::string path;
};
ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}
ControlSocket::~ControlSocket() = default;
bool ControlSocket::running() const { return false; }
const std::string& ControlSocket::path() const { return impl_->path; }
bool ControlSocket::start(const std::string&, std::string* why) {
    if (why) *why = "the control socket is POSIX-only";
    return false;
}
void ControlSocket::stop() {}
std::vector<ControlSocket::Request> ControlSocket::take() { return {}; }
int ControlSocket::readyFd() const { return -1; }
void ControlSocket::reply(uint64_t, bool, std::string) {}

#endif

}  // namespace bro::platform
