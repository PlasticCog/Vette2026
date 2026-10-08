#pragma once
// What the two games of a two-player race tell each other once they're connected, before the original's
// own packets: each player's name, and the host's city map if it plays one of its own (ui/map_editor.h),
// so both race in the same city. It goes down the serial cable first, in each direction, and is taken off
// at the other end before the original game sees anything (it never knows).
//
// On the cable: "VETTE2026 INTRO 1\n", the payload's length in decimal and "\n", then the payload:
// "name=<name>\n", and for a map "map\n" followed by the map's text (CityMap::serialize) to the end.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/city_map.h"
#include "host/serial_link.h"

namespace vette::game {

struct RaceIntro {
    std::string name;            // the player's (cleaned: core/settings.h clean_player_name)
    std::optional<CityMap> map;  // the host's own city, if any

    std::string encode() const;  // the payload
    // A payload: nullopt (with `error`) if it isn't one.
    static std::optional<RaceIntro> decode(std::string_view payload, std::string& error);
};

// The serial cable with the intros taken care of: this game's goes out first, as soon as the cable is
// connected (the game's bytes before that are lost, as on a cable whose other end isn't listening), and
// the other game's is taken off the front of what arrives. The game's own bytes pass through unchanged.
class IntroLink final : public host::SerialLink {
public:
    static constexpr std::size_t kMaxPayload = 256 * 1024;

    IntroLink(host::SerialLink& inner, const RaceIntro& mine);

    // Sends this game's intro once the cable is connected, and reads the other's as it arrives. Call it
    // until received() (send() and receive() call it too).
    void poll();
    bool received() const { return state_ == State::Done && theirs_.has_value(); }
    // The other game's intro wasn't one (another program, or a damaged one): what came is passed to the
    // game as it is.
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    const std::optional<RaceIntro>& theirs() const { return theirs_; }

    // host::SerialLink
    void send(std::span<const std::uint8_t> bytes) override;
    std::size_t receive(std::span<std::uint8_t> out) override;
    bool connected() const override { return inner_.connected(); }

private:
    enum class State { Magic, Length, Payload, Done };
    void fail(const std::string& why);

    host::SerialLink& inner_;
    std::string mine_;  // on the cable
    bool sent_ = false;
    State state_ = State::Magic;
    std::string header_;              // the magic and length lines as they come
    std::size_t length_ = 0;
    std::string payload_;
    std::vector<std::uint8_t> early_;  // bytes to hand the game before reading more (a failed intro's)
    std::optional<RaceIntro> theirs_;
    std::string error_;
};

}  // namespace vette::game
