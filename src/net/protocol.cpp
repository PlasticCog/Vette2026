#include "net/protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace vette::net {

namespace {

char upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

bool key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

bool valid_key(std::string_view key) {
    return !key.empty() && key.size() <= 32 && std::all_of(key.begin(), key.end(), key_char);
}

bool valid_value(std::string_view value) {
    return value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

void append_utf8(std::string& out, std::uint32_t cp) {
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

// A minimal JSON reader for the server's flat objects.
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : s_(text) {}

    std::optional<JsonObject> object() {
        JsonObject result;
        skip_space();
        if (!eat('{')) {
            return std::nullopt;
        }
        skip_space();
        if (eat('}')) {
            return finish(std::move(result));
        }
        for (;;) {
            skip_space();
            auto key = string();
            skip_space();
            if (!key || !eat(':')) {
                return std::nullopt;
            }
            skip_space();
            auto value = scalar();
            if (!value) {
                return std::nullopt;
            }
            result[*key] = std::move(*value);
            skip_space();
            if (eat(',')) {
                continue;
            }
            if (eat('}')) {
                return finish(std::move(result));
            }
            return std::nullopt;
        }
    }

private:
    std::optional<JsonObject> finish(JsonObject result) {
        skip_space();
        return i_ == s_.size() ? std::optional<JsonObject>(std::move(result)) : std::nullopt;
    }

    void skip_space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) {
            ++i_;
        }
    }

    bool eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    std::optional<std::uint32_t> hex4() {
        if (s_.size() - i_ < 4) {
            return std::nullopt;
        }
        std::uint32_t v = 0;
        const auto r = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, v, 16);
        if (r.ptr != s_.data() + i_ + 4) {
            return std::nullopt;
        }
        i_ += 4;
        return v;
    }

    std::optional<std::string> string() {
        if (!eat('"')) {
            return std::nullopt;
        }
        std::string out;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) {
                return std::nullopt;
            }
            switch (s_[i_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = hex4();
                if (!cp) {
                    return std::nullopt;
                }
                if (*cp >= 0xD800 && *cp < 0xDC00 && eat('\\') && eat('u')) {
                    const auto low = hex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF) {
                        return std::nullopt;
                    }
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                }
                append_utf8(out, *cp);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // A string's contents, or a number/true/false/null as written.
    std::optional<std::string> scalar() {
        if (i_ < s_.size() && s_[i_] == '"') {
            return string();
        }
        const std::size_t start = i_;
        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' ||
                                  s_[i_] == '+' || s_[i_] == '.')) {
            ++i_;
        }
        if (i_ == start) {
            return std::nullopt;  // nested objects and arrays aren't part of the protocol
        }
        return std::string(s_.substr(start, i_ - start));
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

}  // namespace

std::optional<std::string> normalize_room_code(std::string_view typed) {
    std::string code;
    for (const char c : typed) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            code += upper(c);
        } else if (c != ' ' && c != '-' && c != '_' && c != '\t') {
            return std::nullopt;
        }
    }
    if (code.size() > kCodeLength && code.starts_with("VETTE")) {
        code.erase(0, 5);
    }
    if (code.size() != kCodeLength) {
        return std::nullopt;
    }
    for (const char c : code) {
        if (kCodeAlphabet.find(c) == std::string_view::npos) {
            return std::nullopt;
        }
    }
    return code;
}

std::string format_room_code(std::string_view code) { return std::string(kCodePrefix) + std::string(code); }

bool RaceSettings::set(std::string_view key, std::string_view value) {
    if (!valid_key(key) || !valid_value(value)) {
        return false;
    }
    RaceSettings next = *this;
    auto it = std::find_if(next.values_.begin(), next.values_.end(),
                           [&](const auto& kv) { return kv.first == key; });
    if (it != next.values_.end()) {
        it->second = value;
    } else {
        next.values_.emplace_back(key, value);
    }
    if (next.serialize().size() > kMaxBytes) {
        return false;
    }
    *this = std::move(next);
    return true;
}

std::optional<std::string> RaceSettings::get(std::string_view key) const {
    for (const auto& [k, v] : values_) {
        if (k == key) {
            return v;
        }
    }
    return std::nullopt;
}

bool RaceSettings::get_bool(std::string_view key, bool fallback) const {
    const auto v = get(key);
    if (!v) {
        return fallback;
    }
    if (*v == "1" || *v == "true" || *v == "on") {
        return true;
    }
    if (*v == "0" || *v == "false" || *v == "off") {
        return false;
    }
    return fallback;
}

std::string RaceSettings::serialize() const {
    std::string out;
    for (const auto& [k, v] : values_) {
        if (!out.empty()) {
            out += '\n';
        }
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

std::optional<RaceSettings> RaceSettings::parse(std::string_view text) {
    RaceSettings s;
    if (text.size() > kMaxBytes) {
        return std::nullopt;
    }
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (line.empty()) {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || !s.set(line.substr(0, eq), line.substr(eq + 1))) {
            return std::nullopt;
        }
    }
    return s;
}

void MessageHeader::append_to(std::vector<std::uint8_t>& out) const {
    out.push_back(kind);
    out.push_back(flags);
    out.push_back(0);
    out.push_back(0);
    put_u32(out, ts_us);
    put_u32(out, echo_ts_us);
    put_u32(out, echo_hold_us);
    put_u32(out, offset);
    put_u32(out, ack);
}

std::optional<MessageHeader> MessageHeader::decode(std::span<const std::uint8_t> message) {
    if (message.size() < kSize || (message[0] != kData && message[0] != kAck)) {
        return std::nullopt;
    }
    MessageHeader h;
    h.kind = message[0];
    h.flags = message[1];
    h.ts_us = get_u32(&message[4]);
    h.echo_ts_us = get_u32(&message[8]);
    h.echo_hold_us = get_u32(&message[12]);
    h.offset = get_u32(&message[16]);
    h.ack = get_u32(&message[20]);
    return h;
}

std::optional<JsonObject> parse_json_object(std::string_view text) { return JsonReader(text).object(); }

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(c >> 4) & 0xF];
                out += kHex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string ServerUrl::host_header() const {
    const std::string name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if ((tls && port == 443) || (!tls && port == 80)) {
        return name;
    }
    return name + ":" + std::to_string(port);
}

std::string ServerUrl::curl_url() const {
    const std::string name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return std::string(tls ? "https://" : "http://") + name + ":" + std::to_string(port) + "/";
}

std::optional<ServerUrl> parse_server_url(std::string_view url) {
    ServerUrl u;
    std::string lower;
    for (const char c : url.substr(0, std::min<std::size_t>(url.size(), 8))) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::size_t skip = 0;
    if (lower.starts_with("wss://")) {
        skip = 6;
    } else if (lower.starts_with("https://")) {
        skip = 8;
    } else if (lower.starts_with("ws://")) {
        u.tls = false;
        skip = 5;
    } else if (lower.starts_with("http://")) {
        u.tls = false;
        skip = 7;
    } else {
        return std::nullopt;
    }
    url.remove_prefix(skip);
    const std::size_t slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view() : url.substr(slash);
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    u.path = path;
    u.port = u.tls ? 443 : 80;
    std::string_view port_text;
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::nullopt;
            }
            port_text = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.find(':');
        u.host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
        }
    }
    if (!port_text.empty()) {
        int port = 0;
        const auto r = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (r.ptr != port_text.data() + port_text.size() || port <= 0 || port > 65535) {
            return std::nullopt;
        }
        u.port = port;
    }
    if (u.host.empty() || u.host.find_first_of(" @?#") != std::string::npos) {
        return std::nullopt;
    }
    return u;
}

std::string percent_encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto b = static_cast<unsigned char>(c);
        if (std::isalnum(b) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[b >> 4];
            out += kHex[b & 0xF];
        }
    }
    return out;
}

}  // namespace vette::net
