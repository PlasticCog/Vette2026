#pragma once
// What players pass each other to race online:
//
//  - a room code, "VETTE-4KQ7": a room on the relay server (net/room_link.h);
//  - a direct code, "7K3M-QX9P-2HDA" (12 characters) or "7K3M-QX9P-2HDA-8RTW" (16): the host's public
//    IPv4 address and port, and a secret, for joining its game straight over the internet with no
//    server (net/direct.h). Same alphabet as room codes (no 0/O, 1/I); the last character is a check
//    character (Luhn mod 32), so a typo is caught before connecting;
//  - an invite link, https://<server>/join/VETTE-4KQ7 or https://<server>/direct/7K3M-QX9P-2HDA, a page
//    on the relay server that opens the game (vette2026://join/VETTE-4KQ7, vette2026://direct/...).
//
// parse_invite() takes any of these as pasted (with or without the dashes, in a sentence, in any case).

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace vette::net {

// The host's port for direct connections (tried first; a 12-character code implies it).
inline constexpr std::uint16_t kDirectPort = 26989;

struct DirectCode {
    std::uint32_t ipv4 = 0;  // host order
    std::uint16_t port = kDirectPort;
    std::uint32_t secret = 0;  // 21 bits with the default port (12 characters), 25 bits otherwise (16)

    static constexpr int kShortSecretBits = 21, kLongSecretBits = 25;

    // A new code for this address and port, with a random secret of the right size.
    static DirectCode make(std::uint32_t ipv4, std::uint16_t port, std::mt19937_64& rng);
    // "7K3M-QX9P-2HDA" (or 16 characters if the port isn't kDirectPort).
    std::string encode() const;
    // nullopt if `typed` isn't a direct code (wrong length or characters, or the check fails).
    static std::optional<DirectCode> decode(std::string_view typed);
    // The secret as the handshake uses it (net/direct.h).
    std::string key() const;

    bool operator==(const DirectCode&) const = default;
};

struct Invite {
    enum class Kind { Room, Direct };
    Kind kind = Kind::Room;
    std::string code;  // as shown: "VETTE-4KQ7", "7K3M-QX9P-2HDA"
};

// Anything a player might paste: an invite link, a vette2026:// link, a room code or a direct code.
std::optional<Invite> parse_invite(std::string_view pasted);

// The https link for an invite through the relay server at `server_url` (wss://... or ws://...);
// without a server, the code itself.
std::string invite_link(std::string_view server_url, const Invite& invite);
// The game's own link: vette2026://join/VETTE-4KQ7, vette2026://direct/7K3M-QX9P-2HDA.
std::string app_link(const Invite& invite);

}  // namespace vette::net
