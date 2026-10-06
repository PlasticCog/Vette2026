#include "net/websocket.h"

#include <cctype>
#include <charconv>
#include <cstring>

namespace vette::net::ws {

namespace {

constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string refusal_reason(int status) {
    switch (status) {
    case 404: return "There's no VETTE! 2026 server at that address (HTTP 404). Check the server setting.";
    case 426: return "The server didn't accept the WebSocket (HTTP 426).";
    case 429:
        return "The online server is busy or has used up today's free allowance (HTTP 429). "
               "Try again later; the allowance resets at midnight UTC.";
    case 500: case 502: case 503: case 504:
        return "The online server has a problem (HTTP " + std::to_string(status) + "). Try again later.";
    default:
        return "The server refused the connection (HTTP " + std::to_string(status) + ").";
    }
}

}  // namespace

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t* p = &msg[chunk + 4 * static_cast<std::size_t>(i)];
            w[i] = static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
                   static_cast<std::uint32_t>(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    }
    return out;
}

std::string base64(std::span<const std::uint8_t> data) {
    static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16 |
                                static_cast<std::uint32_t>(data[i + 1]) << 8 | data[i + 2];
        out += kChars[v >> 18];
        out += kChars[(v >> 12) & 63];
        out += kChars[(v >> 6) & 63];
        out += kChars[v & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out += kChars[v >> 18];
        out += kChars[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16 |
                                static_cast<std::uint32_t>(data[i + 1]) << 8;
        out += kChars[v >> 18];
        out += kChars[(v >> 12) & 63];
        out += kChars[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string handshake_request(std::string_view host_header, std::string_view target, std::string_view key,
                              std::string_view user_agent) {
    std::string r;
    r += "GET ";
    r += target;
    r += " HTTP/1.1\r\nHost: ";
    r += host_header;
    r += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    r += key;
    r += "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ";
    r += user_agent;
    r += "\r\n\r\n";
    return r;
}

std::string accept_key(std::string_view key) {
    std::string text(key);
    text += kGuid;
    const auto digest = sha1({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    return base64(digest);
}

HandshakeResponse parse_handshake_response(std::string_view received, std::string_view key) {
    HandshakeResponse r;
    const std::size_t end = received.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (received.size() > 16384) {
            r.state = HandshakeResponse::State::Refused;
            r.error = "The server's response is too long.";
        }
        return r;
    }
    r.length = end + 4;
    r.state = HandshakeResponse::State::Refused;
    std::string_view head = received.substr(0, end);
    std::size_t eol = head.find("\r\n");
    const std::string_view status_line = head.substr(0, eol);
    // "HTTP/1.1 101 Switching Protocols"
    const std::size_t sp = status_line.find(' ');
    if (!status_line.starts_with("HTTP/") || sp == std::string_view::npos) {
        r.error = "The server didn't answer as a web server would.";
        return r;
    }
    const std::string_view code = status_line.substr(sp + 1, 3);
    if (std::from_chars(code.data(), code.data() + code.size(), r.status).ec != std::errc()) {
        r.error = "The server didn't answer as a web server would.";
        return r;
    }
    if (r.status != 101) {
        r.error = refusal_reason(r.status);
        return r;
    }
    bool upgrade = false, connection = false, accepted = false;
    const std::string expected = accept_key(key);
    while (eol != std::string_view::npos) {
        head.remove_prefix(eol + 2);
        eol = head.find("\r\n");
        const std::string_view line = head.substr(0, eol);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "upgrade") {
            upgrade = lower(value) == "websocket";
        } else if (name == "connection") {
            connection = lower(value).find("upgrade") != std::string::npos;
        } else if (name == "sec-websocket-accept") {
            accepted = value == expected;
        }
    }
    if (!upgrade || !connection || !accepted) {
        r.error = "The server's WebSocket handshake is wrong.";
        return r;
    }
    r.state = HandshakeResponse::State::Accepted;
    return r;
}

void append_frame(std::vector<std::uint8_t>& out, Opcode op, std::span<const std::uint8_t> payload,
                  std::array<std::uint8_t, 4> mask) {
    out.push_back(static_cast<std::uint8_t>(0x80 | op));
    const std::size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<std::uint8_t>(0x80 | n));
    } else if (n <= 0xFFFF) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
    } else {
        out.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(n) >> (8 * i)));
        }
    }
    out.insert(out.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>(payload[i] ^ mask[i & 3]));
    }
}

bool FrameDecoder::feed(std::span<const std::uint8_t> bytes, std::vector<Message>& out) {
    if (!error_.empty()) {
        return false;
    }
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t avail = buf_.size() - pos;
        if (avail < 2) {
            break;
        }
        const std::uint8_t b0 = buf_[pos], b1 = buf_[pos + 1];
        const bool fin = (b0 & 0x80) != 0;
        const auto op = static_cast<Opcode>(b0 & 0x0F);
        if (b0 & 0x70) {
            return fail("The server sent a WebSocket frame with reserved bits set.");
        }
        if (b1 & 0x80) {
            return fail("The server sent a masked WebSocket frame.");
        }
        std::size_t head = 2;
        std::uint64_t len = b1 & 0x7F;
        if (len == 126) {
            head = 4;
            if (avail < head) {
                break;
            }
            len = static_cast<std::uint64_t>(buf_[pos + 2]) << 8 | buf_[pos + 3];
        } else if (len == 127) {
            head = 10;
            if (avail < head) {
                break;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = len << 8 | buf_[pos + 2 + static_cast<std::size_t>(i)];
            }
        }
        const bool control = (op & 0x08) != 0;
        if (control && (!fin || len > 125)) {
            return fail("The server sent a malformed WebSocket control frame.");
        }
        if (op != kContinuation && op != kText && op != kBinary && op != kClose && op != kPing &&
            op != kPong) {
            return fail("The server sent an unknown WebSocket frame type.");
        }
        if (len > max_message_ || partial_.size() + len > max_message_) {
            return fail("The server sent a WebSocket message that is too large.");
        }
        if (avail - head < len) {
            break;
        }
        const auto* payload = buf_.data() + pos + head;
        const auto n = static_cast<std::size_t>(len);
        pos += head + n;
        if (control) {
            out.push_back({op, std::vector<std::uint8_t>(payload, payload + n)});
            continue;
        }
        if (op == kContinuation) {
            if (partial_op_ == kContinuation) {
                return fail("The server sent a WebSocket continuation frame out of place.");
            }
        } else {
            if (partial_op_ != kContinuation) {
                return fail("The server started a WebSocket message inside another.");
            }
            partial_op_ = op;
        }
        partial_.insert(partial_.end(), payload, payload + n);
        if (fin) {
            out.push_back({partial_op_, std::move(partial_)});
            partial_.clear();
            partial_op_ = kContinuation;
        }
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
    return true;
}

}  // namespace vette::net::ws
