#include "game/race_intro.h"

#include <algorithm>
#include <charconv>

namespace vette::game {
namespace {

constexpr std::string_view kMagic = "VETTE2026 INTRO 1\n";
constexpr std::size_t kMaxName = 64;      // decode() keeps no more than this (the sender cleans it)
constexpr std::size_t kMaxLengthLine = 8;  // "262144\n"

}  // namespace

std::string RaceIntro::encode() const {
    std::string out = "name=";
    for (const char c : name) {
        if (c != '\n' && c != '\r') out += c;
    }
    out += '\n';
    if (map) {
        out += "map\n";
        out += map->serialize();
    }
    return out;
}

std::optional<RaceIntro> RaceIntro::decode(std::string_view payload, std::string& error) {
    RaceIntro intro;
    bool named = false;
    while (!payload.empty()) {
        const std::size_t eol = payload.find('\n');
        const std::string_view line = payload.substr(0, eol);
        payload = eol == std::string_view::npos ? std::string_view{} : payload.substr(eol + 1);
        if (line.rfind("name=", 0) == 0) {
            for (const char c : line.substr(5)) {
                if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7E && intro.name.size() < kMaxName)
                    intro.name += c;
            }
            named = true;
        } else if (line == "map") {
            intro.map = CityMap::parse(payload, error);
            if (!intro.map) {
                error = "the host's map: " + error;
                return std::nullopt;
            }
            break;
        }  // other lines: from a later version, ignored
    }
    if (!named) {
        error = "no name";
        return std::nullopt;
    }
    return intro;
}

IntroLink::IntroLink(host::SerialLink& inner, const RaceIntro& mine) : inner_(inner) {
    const std::string payload = mine.encode();
    mine_ = std::string(kMagic) + std::to_string(payload.size()) + "\n" + payload;
}

void IntroLink::fail(const std::string& why) {
    error_ = why;
    state_ = State::Done;
    // What came so far goes to the game, as if there had been no intro.
    early_.insert(early_.end(), header_.begin(), header_.end());
    early_.insert(early_.end(), payload_.begin(), payload_.end());
    header_.clear();
    payload_.clear();
}

void IntroLink::poll() {
    if (!sent_ && inner_.connected()) {
        sent_ = true;
        inner_.send(std::span(reinterpret_cast<const std::uint8_t*>(mine_.data()), mine_.size()));
    }
    std::uint8_t buf[4096];
    while (state_ != State::Done) {
        if (state_ == State::Payload) {
            const std::size_t want = std::min(sizeof buf, length_ - payload_.size());
            const std::size_t n = inner_.receive(std::span(buf, want));
            if (n == 0) return;
            payload_.append(reinterpret_cast<const char*>(buf), n);
            if (payload_.size() == length_) {
                std::string why;
                theirs_ = RaceIntro::decode(payload_, why);
                if (!theirs_) {
                    error_ = "the other game's intro: " + why;  // (its bytes aren't the game's: dropped)
                }
                payload_.clear();
                state_ = State::Done;
            }
            continue;
        }
        // The header, a byte at a time (so nothing after it is taken).
        if (inner_.receive(std::span(buf, 1)) == 0) return;
        header_ += static_cast<char>(buf[0]);
        if (state_ == State::Magic) {
            if (kMagic.compare(0, header_.size(), header_) != 0) {
                fail("the other game sent no intro");
            } else if (header_.size() == kMagic.size()) {
                state_ = State::Length;
            }
        } else if (buf[0] == '\n') {
            std::size_t n = 0;
            const char* digits = header_.data() + kMagic.size();
            const char* last = header_.data() + header_.size() - 1;
            const auto [end, ec] = std::from_chars(digits, last, n);
            if (ec != std::errc{} || end != last || n > kMaxPayload) {
                fail("the other game's intro is damaged");
            } else {
                length_ = n;
                header_.clear();
                state_ = State::Payload;
                if (n == 0) {
                    std::string why;
                    theirs_ = RaceIntro::decode({}, why);
                    if (!theirs_) error_ = "the other game's intro: " + why;
                    state_ = State::Done;
                }
            }
        } else if (header_.size() > kMagic.size() + kMaxLengthLine) {
            fail("the other game's intro is damaged");
        }
    }
}

void IntroLink::send(std::span<const std::uint8_t> bytes) {
    poll();  // (ours first)
    if (sent_)
        inner_.send(bytes);
    // (else the cable isn't connected yet: lost, as on a cable whose other end isn't listening)
}

std::size_t IntroLink::receive(std::span<std::uint8_t> out) {
    if (state_ != State::Done) poll();
    if (state_ != State::Done) return 0;
    if (!early_.empty()) {
        const std::size_t n = std::min(out.size(), early_.size());
        std::copy_n(early_.begin(), n, out.begin());
        early_.erase(early_.begin(), early_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    return inner_.receive(out);
}

}  // namespace vette::game
