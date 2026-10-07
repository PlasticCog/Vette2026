#include "net/invite.h"

#include <cctype>
#include <vector>

#include "net/protocol.h"

namespace vette::net {

namespace {

constexpr int kShortChars = 12, kLongChars = 16;  // with the check character

int symbol(char c) {
    const std::size_t i = kCodeAlphabet.find(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return i == std::string_view::npos ? -1 : static_cast<int>(i);
}

// Luhn mod 32 (the Luhn algorithm over the code's alphabet): catches every single wrong character and
// most swapped neighbours.
int luhn_check(const std::vector<int>& values) {
    int factor = 2, sum = 0;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        int addend = factor * *it;
        factor = factor == 2 ? 1 : 2;
        sum += addend / 32 + addend % 32;
    }
    return (32 - sum % 32) % 32;
}

// The letters and digits of a typed code, upper case; nullopt if there's anything else in it.
std::optional<std::string> compact(std::string_view typed) {
    std::string out;
    for (const char c : typed) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else if (c != '-' && c != ' ' && c != '_' && c != '\t') {
            return std::nullopt;
        }
    }
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool trim_char(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '(' ||
           c == ')' || c == '[' || c == ']' || c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == '`';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && trim_char(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && trim_char(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<Invite> room(std::string_view code) {
    if (auto c = normalize_room_code(code)) {
        return Invite{Invite::Kind::Room, format_room_code(*c)};
    }
    return std::nullopt;
}

std::optional<Invite> direct(std::string_view code) {
    if (auto c = DirectCode::decode(code)) {
        return Invite{Invite::Kind::Direct, c->encode()};
    }
    return std::nullopt;
}

// A link's path from "join/..." or "direct/...": the code is the next segment.
std::optional<Invite> from_path(std::string_view path) {
    const std::string l = lower(path);
    for (const auto& [prefix, kind] : {std::pair{std::string("join/"), Invite::Kind::Room},
                                      std::pair{std::string("direct/"), Invite::Kind::Direct}}) {
        const std::size_t at = l.find(prefix);
        if (at == std::string::npos || (at > 0 && l[at - 1] != '/')) {
            continue;
        }
        std::string_view seg = path.substr(at + prefix.size());
        seg = seg.substr(0, seg.find_first_of("/?#& \t\r\n"));
        return kind == Invite::Kind::Room ? room(seg) : direct(seg);
    }
    return std::nullopt;
}

std::optional<Invite> parse_one(std::string_view text, bool bare_room_ok) {
    text = trim(text);
    const std::string l = lower(text);
    const std::size_t scheme = l.find("vette2026:");
    if (scheme != std::string::npos) {
        return from_path(text.substr(scheme + 10));
    }
    if (l.find("://") != std::string::npos || l.find("/join/") != std::string::npos ||
        l.find("/direct/") != std::string::npos) {
        return from_path(text);
    }
    if (auto d = direct(text)) {
        return d;
    }
    // A bare 4-letter word could be anything in a sentence; on its own, it's a room code.
    if (bare_room_ok || l.starts_with("vette")) {
        return room(text);
    }
    return std::nullopt;
}

std::string base_url(std::string_view server_url) {
    std::string s(server_url);
    const std::string l = lower(s);
    if (l.starts_with("wss://")) {
        s = "https://" + s.substr(6);
    } else if (l.starts_with("ws://")) {
        s = "http://" + s.substr(5);
    }
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

}  // namespace

DirectCode DirectCode::make(std::uint32_t ipv4, std::uint16_t port, std::mt19937_64& rng) {
    DirectCode c;
    c.ipv4 = ipv4;
    c.port = port;
    const int bits = port == kDirectPort ? kShortSecretBits : kLongSecretBits;
    c.secret = static_cast<std::uint32_t>(rng() & ((1u << bits) - 1));
    return c;
}

std::string DirectCode::encode() const {
    const bool shortform = port == kDirectPort && secret < (1u << kShortSecretBits);
    // Bits, most significant first: format (2), address (32), [port (16)], secret.
    std::vector<int> bits;
    auto put = [&](std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits.push_back(static_cast<int>((v >> i) & 1));
        }
    };
    put(shortform ? 1 : 2, 2);
    put(ipv4, 32);
    if (!shortform) {
        put(port, 16);
    }
    put(secret, shortform ? kShortSecretBits : kLongSecretBits);
    std::vector<int> values;
    for (std::size_t i = 0; i < bits.size(); i += 5) {
        int v = 0;
        for (std::size_t j = 0; j < 5; ++j) {
            v = v << 1 | (i + j < bits.size() ? bits[i + j] : 0);
        }
        values.push_back(v);
    }
    values.push_back(luhn_check(values));
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0 && i % 4 == 0) {
            out += '-';
        }
        out += kCodeAlphabet[static_cast<std::size_t>(values[i])];
    }
    return out;
}

std::optional<DirectCode> DirectCode::decode(std::string_view typed) {
    const auto c = compact(typed);
    if (!c || (c->size() != kShortChars && c->size() != kLongChars)) {
        return std::nullopt;
    }
    std::vector<int> values;
    for (const char ch : *c) {
        const int v = symbol(ch);
        if (v < 0) {
            return std::nullopt;
        }
        values.push_back(v);
    }
    const int check = values.back();
    values.pop_back();
    if (luhn_check(values) != check) {
        return std::nullopt;
    }
    std::size_t pos = 0;
    auto get = [&](int n) {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            v = v << 1 | static_cast<std::uint64_t>((values[pos / 5] >> (4 - pos % 5)) & 1);
        }
        return v;
    };
    const bool shortform = c->size() == kShortChars;
    if (get(2) != (shortform ? 1u : 2u)) {
        return std::nullopt;
    }
    DirectCode code;
    code.ipv4 = static_cast<std::uint32_t>(get(32));
    code.port = shortform ? kDirectPort : static_cast<std::uint16_t>(get(16));
    code.secret = static_cast<std::uint32_t>(get(shortform ? kShortSecretBits : kLongSecretBits));
    if (code.port == 0) {
        return std::nullopt;
    }
    return code;
}

std::string DirectCode::key() const { return "code:" + std::to_string(secret); }

std::optional<Invite> parse_invite(std::string_view pasted) {
    if (auto whole = parse_one(pasted, true)) {
        return whole;
    }
    // In a sentence ("Join me: https://.../join/VETTE-4KQ7 !"): the first word that's a link or a code.
    std::size_t i = 0;
    while (i < pasted.size()) {
        while (i < pasted.size() && std::isspace(static_cast<unsigned char>(pasted[i]))) {
            ++i;
        }
        std::size_t j = i;
        while (j < pasted.size() && !std::isspace(static_cast<unsigned char>(pasted[j]))) {
            ++j;
        }
        if (j > i) {
            if (auto word = parse_one(pasted.substr(i, j - i), false)) {
                return word;
            }
        }
        i = j;
    }
    return std::nullopt;
}

std::string invite_link(std::string_view server_url, const Invite& invite) {
    if (server_url.empty()) {
        return invite.code;
    }
    return base_url(server_url) + (invite.kind == Invite::Kind::Room ? "/join/" : "/direct/") + invite.code;
}

std::string app_link(const Invite& invite) {
    return std::string("vette2026://") + (invite.kind == Invite::Kind::Room ? "join/" : "direct/") + invite.code;
}

}  // namespace vette::net
