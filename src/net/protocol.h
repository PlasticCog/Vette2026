#pragma once
// The online two-player protocol, version 1 (server/README.md has all of it; server/src/index.js is the
// server): room codes, the race settings the host gives the guest, the server's JSON control messages,
// and the header on every binary message between the two games.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vette::net {

inline constexpr int kProtocolVersion = 1;

// --- Room codes: 4 characters from an alphabet without 0/O and 1/I, shown as "VETTE-4KQ7" -----------
inline constexpr std::string_view kCodeAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
inline constexpr std::size_t kCodeLength = 4;
inline constexpr std::string_view kCodePrefix = "VETTE-";

// What a player typed ("vette-4kq7", " 4KQ7", "VETTE 4KQ7") as the bare code ("4KQ7"); nullopt if it
// can't be a code.
std::optional<std::string> normalize_room_code(std::string_view typed);
// "4KQ7" -> "VETTE-4KQ7"
std::string format_room_code(std::string_view code);

// --- Race settings ------------------------------------------------------------------------------------
// Short key=value lines the host gives the guest before the serial stream starts, so both games race
// with the same options (e.g. "improved_driving=1"). The server keeps the newest and gives them to the
// guest in its welcome, ahead of any relayed byte; later changes follow, in order with the stream.
// Keys: 1-32 of [a-z0-9_.-]. Values: up to 64 printable ASCII characters. The whole text (lines joined
// by '\n') is at most kMaxBytes. The format belongs to the protocol version.
class RaceSettings {
public:
    static constexpr std::size_t kMaxBytes = 512;

    // Adds or replaces a value; false if the key or value isn't allowed or the text would be too long.
    bool set(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key) const;
    bool get_bool(std::string_view key, bool fallback) const;  // "1"/"0", "true"/"false", "on"/"off"
    const std::vector<std::pair<std::string, std::string>>& values() const { return values_; }
    bool empty() const { return values_.empty(); }

    std::string serialize() const;
    static std::optional<RaceSettings> parse(std::string_view text);  // nullopt if malformed

    bool operator==(const RaceSettings&) const = default;

private:
    std::vector<std::pair<std::string, std::string>> values_;
};

// --- The header on every binary message between the two games --------------------------------------
// Little-endian, 24 bytes, then (for data) the serial bytes. The server relays these unchanged.
//
// Each game's serial output is one byte stream; `offset` places a data message's bytes in it and `ack`
// tells the other game how much of its stream has arrived. After a reconnect, each side resends what
// wasn't acknowledged and the receiver drops what it already has, so the stream stays intact.
//
// Round-trip time without extra messages: each message carries the sender's clock (`ts_us`), the newest
// `ts_us` it has received from the other game (`echo_ts_us`), and how long ago that arrived
// (`echo_hold_us`). The other game's RTT is then its clock now, minus `echo_ts_us`, minus the hold.
struct MessageHeader {
    enum Kind : std::uint8_t { kData = 1, kAck = 2 };
    static constexpr std::size_t kSize = 24;
    static constexpr std::uint8_t kResend = 1;  // flag: data from `ack` on is missing; send it again
    static constexpr std::uint32_t kNoEcho = 0xFFFFFFFF;

    std::uint8_t kind = kData;
    std::uint8_t flags = 0;
    std::uint32_t ts_us = 0;
    std::uint32_t echo_ts_us = 0;
    std::uint32_t echo_hold_us = kNoEcho;
    std::uint32_t offset = 0;  // the stream position of the payload's first byte (low 32 bits)
    std::uint32_t ack = 0;     // the other game's stream received so far (low 32 bits)

    void append_to(std::vector<std::uint8_t>& out) const;
    static std::optional<MessageHeader> decode(std::span<const std::uint8_t> message);

    bool operator==(const MessageHeader&) const = default;
};

// --- The server's control messages ---------------------------------------------------------------------
// A flat JSON object (string, number, true/false/null values; no nesting) as key -> value text.
using JsonObject = std::map<std::string, std::string, std::less<>>;
std::optional<JsonObject> parse_json_object(std::string_view text);
std::string json_quote(std::string_view text);  // a JSON string literal

// --- The server's address ------------------------------------------------------------------------------
struct ServerUrl {
    bool tls = true;          // wss:// (or https://); ws:// for a local test server
    std::string host;         // name or address (IPv6 without brackets)
    int port = 443;
    std::string path;         // any path prefix, without a trailing '/'

    std::string host_header() const;  // "host" or "host:port" (default ports omitted)
    std::string curl_url() const;     // https://host:port/ (curl makes the TCP/TLS connection)
};
std::optional<ServerUrl> parse_server_url(std::string_view url);

std::string percent_encode(std::string_view text);

}  // namespace vette::net
