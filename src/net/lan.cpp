#include "net/lan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>

#include "net/invite.h"

namespace vette::net {
namespace {

// "VETTE2026 LAN/1?" asks; the answer is the same header, "key=value" lines, an empty line, then the
// race settings (RaceSettings::serialize).
constexpr std::string_view kQuestion = "VETTE2026 LAN/1?\n";
constexpr std::string_view kAnswer = "VETTE2026 LAN/1\n";
constexpr std::int64_t kAskEveryMs = 1000;
constexpr std::int64_t kForgetAfterMs = 3500;  // three questions without an answer: it's gone
constexpr std::size_t kMaxName = 40;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::span<const std::uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// A value for a line: printable ASCII only, so a name can't break the message.
std::string clean(std::string_view s, std::size_t max) {
    std::string out;
    for (const char c : s) {
        if (out.size() < max && c >= 0x20 && c < 0x7F)
            out += c;
    }
    return out;
}

template <typename T>
bool number(std::string_view text, T& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

}  // namespace

std::string LanRace::code() const {
    DirectCode c;
    c.ipv4 = host.ipv4();
    c.port = host.port;
    c.secret = secret;
    return c.encode();
}

std::string lan_question() { return std::string(kQuestion); }

std::string lan_answer(const LanRace& race) {
    std::string m(kAnswer);
    m += "port=" + std::to_string(race.host.port) + "\n";
    m += "secret=" + std::to_string(race.secret) + "\n";
    m += "name=" + clean(race.name, kMaxName) + "\n";
    m += "app=" + clean(race.app_version, kMaxName) + "\n";
    m += "game=" + clean(race.game_build, kMaxName) + "\n";
    m += "\n";
    m += race.settings.serialize();
    return m;
}

std::optional<LanRace> parse_lan_answer(std::string_view m, const SocketAddress& from) {
    if (m.substr(0, kAnswer.size()) != kAnswer || from.v6)
        return std::nullopt;
    m.remove_prefix(kAnswer.size());
    LanRace race;
    bool have_port = false, have_secret = false;
    for (;;) {
        const std::size_t eol = m.find('\n');
        if (eol == std::string_view::npos)
            return std::nullopt;  // no end to the header
        const std::string_view line = m.substr(0, eol);
        m.remove_prefix(eol + 1);
        if (line.empty())
            break;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "port") {
            have_port = number(value, race.host.port) && race.host.port != 0;
        } else if (key == "secret") {
            have_secret = number(value, race.secret);
        } else if (key == "name") {
            race.name = clean(value, kMaxName);
        } else if (key == "app") {
            race.app_version = clean(value, kMaxName);
        } else if (key == "game") {
            race.game_build = clean(value, kMaxName);
        }
    }
    const auto settings = RaceSettings::parse(m);
    if (!have_port || !have_secret || !settings)
        return std::nullopt;
    race.settings = *settings;
    race.host = SocketAddress::ipv4(from.ipv4(), race.host.port);
    return race;
}

bool LanHost::start(std::string& error, std::uint16_t port, bool loopback_only) {
    socket_ = Socket::udp_lan(loopback_only ? 0x7F000001 : 0, port, error);
    return socket_.valid();
}

void LanHost::set_race(const LanRace& race) { answer_ = lan_answer(race); }

void LanHost::poll() {
    if (!socket_.valid() || answer_.empty())
        return;
    std::array<std::uint8_t, 256> buf{};
    SocketAddress from;
    for (std::size_t n; (n = socket_.receive_from(buf, from)) > 0;) {
        const std::string_view m(reinterpret_cast<const char*>(buf.data()), n);
        if (m == kQuestion && !from.v6)
            socket_.send_to(from, bytes(answer_));
    }
}

bool LanSearch::start(std::string& error, std::uint16_t port) {
    port_ = port;
    sockets_.clear();
    Socket any = Socket::udp_lan(0, 0, error);
    if (!any.valid())
        return false;
    sockets_.push_back(std::move(any));
    // With several networks (Wi-Fi and cable, a VPN, a virtual machine's), a broadcast leaves through one
    // of them only: ask on each, from its own address.
    for (const SocketAddress& a : local_addresses()) {
        std::string ignored;
        if (!a.v6 && ipv4_private(a.ipv4())) {
            if (Socket s = Socket::udp_lan(a.ipv4(), 0, ignored); s.valid())
                sockets_.push_back(std::move(s));
        }
    }
    next_ask_ms_ = 0;
    return true;
}

void LanSearch::ask() {
    const auto q = bytes(kQuestion);
    for (Socket& s : sockets_)
        s.send_to(SocketAddress::ipv4(0xFFFFFFFF, port_), q);
    sockets_.front().send_to(SocketAddress::ipv4(0x7F000001, port_), q);  // a race on this computer
}

void LanSearch::poll() {
    if (sockets_.empty())
        return;
    const std::int64_t now = now_ms();
    if (now >= next_ask_ms_) {
        ask();
        next_ask_ms_ = now + kAskEveryMs;
    }
    std::array<std::uint8_t, 2048> buf{};
    SocketAddress from;
    for (Socket& s : sockets_) {
        for (std::size_t n; (n = s.receive_from(buf, from)) > 0;) {
            auto race = parse_lan_answer({reinterpret_cast<const char*>(buf.data()), n}, from);
            if (!race)
                continue;
            race->heard_ms = now;
            // The same race heard on several ways (this computer's own, through the loopback and the
            // network): one entry, at its address on the network if it was heard there.
            const auto same = std::find_if(races_.begin(), races_.end(), [&](const LanRace& r) {
                return r.secret == race->secret && r.host.port == race->host.port && r.name == race->name;
            });
            if (same == races_.end()) {
                races_.push_back(*race);
            } else {
                const bool loopback = (race->host.ipv4() >> 24) == 127;
                if (!loopback)
                    same->host = race->host;
                same->settings = race->settings;
                same->heard_ms = now;
            }
        }
    }
    races_.erase(std::remove_if(races_.begin(), races_.end(),
                                [&](const LanRace& r) { return now - r.heard_ms > kForgetAfterMs; }),
                 races_.end());
}

std::vector<LanRace> LanSearch::races() const { return races_; }

}  // namespace vette::net
