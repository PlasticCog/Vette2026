#pragma once
// One running game takes the invite links. A vette2026:// link starts a new copy of the program (Windows,
// Linux); if a game is already running, the new copy hands it the link over this computer's loopback
// network and exits, and the running game joins (macOS sends links to the running app itself).
// Nothing leaves the computer: the channel listens on 127.0.0.1 only.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/socket.h"

namespace vette::net {

inline constexpr std::uint16_t kInstancePort = 26988;

class InstanceChannel {
public:
    // The program's channel (one per process).
    static InstanceChannel& get();

    // Hands `link` to the game already running, if there is one. True: it took it (this copy can exit).
    // Waits up to `timeout_ms` for the answer.
    static bool forward(std::string_view link, int timeout_ms = 1500);

    // Becomes the game that takes the links. False if another game already is (or the port is in use).
    bool listen();
    // A link handed over since the last call, if any (never blocks).
    std::optional<std::string> take();
    // Stops taking links (before this game makes way for a new copy).
    void close();

private:
    struct Client {
        Socket socket;
        std::string line;
    };
    Socket listener_;
    std::vector<Client> clients_;
};

}  // namespace vette::net
