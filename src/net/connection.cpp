#include "net/connection.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace vette::net {

namespace {

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);  // on Windows this also starts Winsock
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    });
}

CURL* as_curl(void* p) { return static_cast<CURL*>(p); }

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int abort_check(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* abort = static_cast<const std::atomic<bool>*>(clientp);
    return abort && abort->load() ? 1 : 0;
}

std::string connect_error(CURLcode rc, const char* detail, const ServerUrl& server) {
    std::string why;
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
        why = "Can't find the server " + server.host + " (no internet connection, or a wrong address).";
        break;
    case CURLE_COULDNT_CONNECT: why = "Can't connect to the server " + server.host + "."; break;
    case CURLE_OPERATION_TIMEDOUT: why = "The server " + server.host + " didn't answer in time."; break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
        why = "The server's certificate couldn't be verified.";
        break;
    case CURLE_SSL_CONNECT_ERROR: why = "The secure connection to the server failed."; break;
    case CURLE_ABORTED_BY_CALLBACK: why = "Cancelled."; break;
    default: why = std::string("Can't connect to the server: ") + curl_easy_strerror(rc) + "."; break;
    }
    if (detail && *detail && rc != CURLE_ABORTED_BY_CALLBACK) {
        why += " (";
        why += detail;
        while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) {
            why.pop_back();
        }
        why += ")";
    }
    return why;
}

}  // namespace

// --- Waker ---------------------------------------------------------------------------------------------

#if defined(_WIN32)

// A UDP socket on the loopback interface, sending to itself: WSAPoll can't wait on anything but sockets.
Waker::Waker() {
    global_init();
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof addr;
    u_long nonblocking = 1;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        ::closesocket(s);
        return;
    }
    read_ = write_ = static_cast<std::uintptr_t>(s);
    ok_ = true;
}

Waker::~Waker() {
    if (ok_) {
        ::closesocket(static_cast<SOCKET>(read_));
    }
}

void Waker::wake() {
    if (ok_) {
        ::send(static_cast<SOCKET>(write_), "w", 1, 0);
    }
}

void Waker::drain() {
    char buf[64];
    while (ok_ && ::recv(static_cast<SOCKET>(read_), buf, sizeof buf, 0) > 0) {
    }
}

#else

Waker::Waker() {
    global_init();
    int fds[2];
    if (::pipe(fds) != 0) {
        return;
    }
    for (const int fd : fds) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    read_ = static_cast<std::uintptr_t>(fds[0]);
    write_ = static_cast<std::uintptr_t>(fds[1]);
    ok_ = true;
}

Waker::~Waker() {
    if (ok_) {
        ::close(static_cast<int>(read_));
        ::close(static_cast<int>(write_));
    }
}

void Waker::wake() {
    if (ok_) {
        [[maybe_unused]] const auto n = ::write(static_cast<int>(write_), "w", 1);
    }
}

void Waker::drain() {
    char buf[64];
    while (ok_ && ::read(static_cast<int>(read_), buf, sizeof buf) > 0) {
    }
}

#endif

// --- WebSocketConnection ------------------------------------------------------------------------------

WebSocketConnection::WebSocketConnection() : rng_(std::random_device{}()) { global_init(); }

WebSocketConnection::~WebSocketConnection() { close(); }

std::uintptr_t WebSocketConnection::socket() const {
    curl_socket_t s = CURL_SOCKET_BAD;
    if (curl_) {
        curl_easy_getinfo(as_curl(curl_), CURLINFO_ACTIVESOCKET, &s);
    }
    return static_cast<std::uintptr_t>(s);
}

bool WebSocketConnection::connect(const ServerUrl& server, const std::string& target,
                                  const ConnectOptions& options, std::string& error) {
    close();
    CURL* c = curl_easy_init();
    if (!c) {
        error = "Can't start a connection (libcurl).";
        return false;
    }
    char detail[CURL_ERROR_SIZE] = {};
    const std::string url = server.curl_url();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);  // TCP and TLS only: the WebSocket is ours
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));  // ALPN http/1.1
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connect_timeout_ms));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_check);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(options.abort));
#if defined(_WIN32)
    // Schannel checks certificate revocation; don't fail when the revocation server can't be reached.
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    // TLS 1.2 at most: with TLS 1.3 (Windows 11, Server 2022) the session tickets that arrive after the
    // handshake fail Schannel's reads in curl's connect-only mode, and the connection drops.
    curl_easy_setopt(c, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
#endif
    if (!options.ca_file.empty()) {
        curl_easy_setopt(c, CURLOPT_CAINFO, options.ca_file.c_str());
    } else {
        const CaPaths ca = find_system_ca();
        if (!ca.file.empty()) {
            curl_easy_setopt(c, CURLOPT_CAINFO, ca.file.c_str());
        }
        if (!ca.dir.empty()) {
            curl_easy_setopt(c, CURLOPT_CAPATH, ca.dir.c_str());
        }
    }
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, nullptr);
    if (rc != CURLE_OK) {
        error = connect_error(rc, detail, server);
        curl_easy_cleanup(c);
        return false;
    }
    curl_ = c;

    // The WebSocket handshake.
    std::uint8_t nonce[16];
    for (auto& b : nonce) {
        b = static_cast<std::uint8_t>(rng_());
    }
    const std::string key = ws::base64(nonce);
    const std::string request =
        ws::handshake_request(server.host_header(), server.path + target, key, options.user_agent);
    out_.assign(request.begin(), request.end());
    out_pos_ = 0;
    decoder_ = ws::FrameDecoder();
    pending_.clear();
    std::string response;
    const std::int64_t deadline = now_ms() + options.handshake_timeout_ms;
    Waker none;
    for (;;) {
        if (options.abort && options.abort->load()) {
            error = "Cancelled.";
            close();
            return false;
        }
        if (!flush(error)) {
            close();
            return false;
        }
        std::uint8_t buf[4096];
        for (;;) {
            std::size_t n = 0;
            const CURLcode r = curl_easy_recv(c, buf, sizeof buf, &n);
            if (r == CURLE_AGAIN) {
                break;
            }
            if (r != CURLE_OK || n == 0) {
                error = "The server closed the connection during the WebSocket handshake";
                error += r != CURLE_OK ? std::string(" (") + curl_easy_strerror(r) + ")." : ".";
                close();
                return false;
            }
            response.append(reinterpret_cast<const char*>(buf), n);
        }
        const auto answer = ws::parse_handshake_response(response, key);
        if (answer.state == ws::HandshakeResponse::State::Accepted) {
            // Frames may have come with the response (the server's welcome usually does).
            const std::string_view rest = std::string_view(response).substr(answer.length);
            if (!decoder_.feed({reinterpret_cast<const std::uint8_t*>(rest.data()), rest.size()}, pending_)) {
                error = decoder_.error();
                close();
                return false;
            }
            return true;
        }
        if (answer.state == ws::HandshakeResponse::State::Refused) {
            error = answer.error;
            close();
            return false;
        }
        const std::int64_t left = deadline - now_ms();
        if (left <= 0) {
            error = "The server didn't answer the WebSocket handshake.";
            close();
            return false;
        }
        wait(none, static_cast<int>(std::min<std::int64_t>(left, 50)));
    }
}

void WebSocketConnection::queue(ws::Opcode op, std::span<const std::uint8_t> payload) {
    if (!curl_) {
        return;
    }
    if (out_pos_ == out_.size()) {
        out_.clear();
        out_pos_ = 0;
    }
    const std::uint32_t m = rng_();
    ws::append_frame(out_, op, payload,
                     {static_cast<std::uint8_t>(m), static_cast<std::uint8_t>(m >> 8),
                      static_cast<std::uint8_t>(m >> 16), static_cast<std::uint8_t>(m >> 24)});
}

void WebSocketConnection::send_text(std::string_view text) {
    queue(ws::kText, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void WebSocketConnection::send_binary(std::span<const std::uint8_t> data) { queue(ws::kBinary, data); }

bool WebSocketConnection::flush(std::string& error) {
    while (curl_ && out_pos_ < out_.size()) {
        std::size_t n = 0;
        const CURLcode r = curl_easy_send(as_curl(curl_), out_.data() + out_pos_, out_.size() - out_pos_, &n);
        if (r == CURLE_AGAIN) {
            break;
        }
        if (r != CURLE_OK) {
            error = std::string("The connection to the server broke (") + curl_easy_strerror(r) + ").";
            return false;
        }
        out_pos_ += n;
    }
    if (out_pos_ == out_.size()) {
        out_.clear();
        out_pos_ = 0;
    } else if (out_pos_ > 65536) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_pos_));
        out_pos_ = 0;
    }
    return true;
}

bool WebSocketConnection::read(std::vector<ws::FrameDecoder::Message>& out, std::string& error) {
    for (auto& m : pending_) {
        out.push_back(std::move(m));
    }
    pending_.clear();
    if (!curl_) {
        error = "Not connected.";
        return false;
    }
    std::uint8_t buf[16384];
    std::vector<ws::FrameDecoder::Message> frames;
    for (;;) {
        std::size_t n = 0;
        const CURLcode r = curl_easy_recv(as_curl(curl_), buf, sizeof buf, &n);
        if (r == CURLE_AGAIN) {
            return true;
        }
        if (r != CURLE_OK || n == 0) {
            error = r != CURLE_OK
                        ? std::string("The connection to the server broke (") + curl_easy_strerror(r) + ")."
                        : std::string("The server closed the connection.");
            close();
            return false;
        }
        frames.clear();
        if (!decoder_.feed({buf, n}, frames)) {
            error = decoder_.error();
            close();
            return false;
        }
        for (auto& f : frames) {
            switch (f.op) {
            case ws::kPing: queue(ws::kPong, f.data); break;
            case ws::kPong: break;
            case ws::kClose: {
                int code = 1005;
                std::string reason;
                if (f.data.size() >= 2) {
                    code = f.data[0] << 8 | f.data[1];
                    reason.assign(f.data.begin() + 2, f.data.end());
                }
                const std::uint8_t reply[2] = {static_cast<std::uint8_t>(code >> 8),
                                               static_cast<std::uint8_t>(code)};
                queue(ws::kClose, code == 1005 ? std::span<const std::uint8_t>() : reply);
                std::string ignored;
                flush(ignored);
                error = "The server closed the connection (" + std::to_string(code) +
                        (reason.empty() ? "" : " " + reason) + ").";
                close();
                return false;
            }
            default: out.push_back(std::move(f)); break;
            }
        }
    }
}

void WebSocketConnection::wait(Waker& waker, int timeout_ms) {
    const auto sock = static_cast<curl_socket_t>(socket());
    const bool have_sock = curl_ && sock != CURL_SOCKET_BAD;
#if defined(_WIN32)
    WSAPOLLFD fds[2] = {};
    ULONG n = 0;
    if (waker.ok()) {
        fds[n].fd = static_cast<SOCKET>(waker.handle());
        fds[n++].events = POLLRDNORM;
    }
    if (have_sock) {
        fds[n].fd = sock;
        fds[n++].events = static_cast<SHORT>(POLLRDNORM | (wants_write() ? POLLWRNORM : 0));
    }
    if (n == 0) {
        ::Sleep(static_cast<DWORD>(timeout_ms));
        return;
    }
    ::WSAPoll(fds, n, timeout_ms);
#else
    pollfd fds[2] = {};
    nfds_t n = 0;
    if (waker.ok()) {
        fds[n].fd = static_cast<int>(waker.handle());
        fds[n++].events = POLLIN;
    }
    if (have_sock) {
        fds[n].fd = sock;
        fds[n++].events = static_cast<short>(POLLIN | (wants_write() ? POLLOUT : 0));
    }
    ::poll(fds, n, timeout_ms);
#endif
    waker.drain();
}

void WebSocketConnection::shutdown(int timeout_ms) {
    if (!curl_) {
        return;
    }
    const std::uint8_t normal[2] = {0x03, 0xE8};  // 1000: normal closure
    queue(ws::kClose, normal);
    const std::int64_t deadline = now_ms() + timeout_ms;
    std::string error;
    Waker none;
    while (curl_ && wants_write() && now_ms() < deadline && flush(error)) {
        if (wants_write()) {
            wait(none, 10);
        }
    }
    close();
}

void WebSocketConnection::close() {
    if (curl_) {
        curl_easy_cleanup(as_curl(curl_));
        curl_ = nullptr;
    }
    out_.clear();
    out_pos_ = 0;
}

// --- Trusted certificates --------------------------------------------------------------------------

CaPaths find_system_ca() {
    CaPaths ca;
#if !defined(_WIN32) && !defined(__APPLE__)
    // OpenSSL is linked into the program, so its built-in paths are the build machine's (Ubuntu's).
    // SSL_CERT_FILE and SSL_CERT_DIR override, as with OpenSSL itself.
    std::error_code ec;
    const char* env_file = std::getenv("SSL_CERT_FILE");
    const char* env_dir = std::getenv("SSL_CERT_DIR");
    if (env_file && std::filesystem::is_regular_file(env_file, ec)) {
        ca.file = env_file;
    }
    if (env_dir && std::filesystem::is_directory(env_dir, ec)) {
        ca.dir = env_dir;
    }
    static const char* const kFiles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL, CentOS
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/pki/tls/cacert.pem",                            // OpenELEC
        "/etc/ssl/cert.pem",                                  // Alpine, Void
        "/var/lib/ca-certificates/ca-bundle.pem",             // openSUSE (older)
    };
    static const char* const kDirs[] = {"/etc/ssl/certs", "/etc/pki/tls/certs"};
    if (ca.file.empty()) {
        for (const char* f : kFiles) {
            if (std::filesystem::is_regular_file(f, ec)) {
                ca.file = f;
                break;
            }
        }
    }
    if (ca.dir.empty()) {
        for (const char* d : kDirs) {
            if (std::filesystem::is_directory(d, ec)) {
                ca.dir = d;
                break;
            }
        }
    }
#endif
    return ca;
}

}  // namespace vette::net
