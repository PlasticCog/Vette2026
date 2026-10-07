#include "core/settings.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace vette {
namespace {

// Each setting's name in the file and its value names, in enum order.
template <typename Enum, size_t N>
struct Choice {
    std::string_view key;
    std::array<std::string_view, N> names;
};

constexpr Choice<Settings::FrameRate, 2> kFrameRate{"frame_rate", {"smooth", "original"}};
constexpr Choice<Settings::Pc, 2> kPc{"pc", {"fast", "286"}};
constexpr Choice<Settings::DrawDistance, 3> kDrawDistance{"draw_distance", {"original", "extended", "maximum"}};
constexpr Choice<Settings::Joystick, 3> kJoystick{"joystick", {"auto", "on", "off"}};
constexpr Choice<Settings::Effects, 4> kEffects{"effects", {"off", "speaker", "adlib", "mac"}};
constexpr Choice<Settings::Music, 3> kMusic{"music", {"off", "original", "pc98"}};
constexpr Choice<Settings::Graphics, 3> kGraphics{"graphics", {"dos", "pc98", "mac"}};
constexpr std::array<std::string_view, 2> kManualCheck{"skip", "show"};
constexpr std::array<std::string_view, 2> kDisplay{"window", "fullscreen"};
constexpr Choice<Settings::Scaling, 2> kScaling{"scaling", {"sharp", "smooth"}};
constexpr Choice<Settings::Skyline, 2> kSkyline{"skyline", {"hills", "painted"}};
constexpr Choice<Settings::ViewResolution, 2> kViewResolution{"view_resolution", {"display", "original"}};
constexpr std::array<std::string_view, 2> kOnOff{"off", "on"};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

template <typename Enum, size_t N>
void read(const Choice<Enum, N>& choice, std::string_view key, std::string_view value, Enum& out) {
    if (key != choice.key)
        return;
    for (size_t i = 0; i < N; ++i) {
        if (value == choice.names[i])
            out = static_cast<Enum>(i);
    }
}

void read_bool(std::string_view expected, const std::array<std::string_view, 2>& names, std::string_view key,
               std::string_view value, bool& out) {
    if (key == expected && (value == names[0] || value == names[1]))
        out = value == names[1];
}

}  // namespace

Settings::Preset Settings::preset() const {
    for (const Preset p : {Preset::Classic, Preset::Enhanced}) {
        Settings s = *this;
        s.apply(p);
        if (s == *this)
            return p;
    }
    return Preset::Custom;
}

void Settings::apply(Preset p) {
    if (p == Preset::Classic) {
        frame_rate = FrameRate::Original;
        pc = Pc::At286;
        draw_distance = DrawDistance::Original;
        effects = Effects::Speaker;
        music = Music::Original;
        manual_check = true;
        improved_driving = false;
        lane_centering = false;
        smooth_traffic = false;
    } else if (p == Preset::Enhanced) {
        frame_rate = FrameRate::Smooth;
        pc = Pc::Fast;
        draw_distance = DrawDistance::Maximum;
        effects = Effects::AdLib;
        music = Music::Original;
        manual_check = false;
        smooth_traffic = true;
    }
}

std::string Settings::serialize() const {
    std::ostringstream out;
    out << "# VETTE! 2026 settings (edited by the launch menu)\n";
    out << kFrameRate.key << " = " << kFrameRate.names[static_cast<size_t>(frame_rate)] << "\n";
    out << kPc.key << " = " << kPc.names[static_cast<size_t>(pc)] << "\n";
    out << kDrawDistance.key << " = " << kDrawDistance.names[static_cast<size_t>(draw_distance)] << "\n";
    out << "manual_check = " << kManualCheck[manual_check] << "\n";
    out << kJoystick.key << " = " << kJoystick.names[static_cast<size_t>(joystick)] << "\n";
    out << "display = " << kDisplay[fullscreen] << "\n";
    out << kScaling.key << " = " << kScaling.names[static_cast<size_t>(scaling)] << "\n";
    out << kSkyline.key << " = " << kSkyline.names[static_cast<size_t>(skyline)] << "\n";
    out << kViewResolution.key << " = " << kViewResolution.names[static_cast<size_t>(view_resolution)] << "\n";
    out << "depth_buffer = " << kOnOff[depth_buffer] << "\n";
    out << "improved_driving = " << kOnOff[improved_driving] << "\n";
    out << "lane_centering = " << kOnOff[lane_centering] << "\n";
    out << "smooth_traffic = " << kOnOff[smooth_traffic] << "\n";
    out << "map = " << map_name << "\n";
    out << kEffects.key << " = " << kEffects.names[static_cast<size_t>(effects)] << "\n";
    out << kMusic.key << " = " << kMusic.names[static_cast<size_t>(music)] << "\n";
    out << kGraphics.key << " = " << kGraphics.names[static_cast<size_t>(graphics)] << "\n";
    out << "launcher = " << kOnOff[show_launcher] << "\n";
    out << "game_folder = " << game_folder << "\n";
    out << "online_course = " << online_course << "\n";
    out << "online_port_forwarded = " << kOnOff[online_port_forwarded] << "\n";
    out << "online_address = " << online_address << "\n";
    return out.str();
}

Settings Settings::parse(const std::string& text) {
    Settings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view l = trim(line);
        const size_t eq = l.find('=');
        if (l.empty() || l.front() == '#' || eq == std::string_view::npos)
            continue;
        const std::string_view key = trim(l.substr(0, eq));
        const std::string_view value = trim(l.substr(eq + 1));
        read(kFrameRate, key, value, s.frame_rate);
        read(kPc, key, value, s.pc);
        read(kDrawDistance, key, value, s.draw_distance);
        read(kJoystick, key, value, s.joystick);
        read_bool("manual_check", kManualCheck, key, value, s.manual_check);
        read_bool("display", kDisplay, key, value, s.fullscreen);
        read(kScaling, key, value, s.scaling);
        read(kSkyline, key, value, s.skyline);
        read(kViewResolution, key, value, s.view_resolution);
        read_bool("depth_buffer", kOnOff, key, value, s.depth_buffer);
        read_bool("improved_driving", kOnOff, key, value, s.improved_driving);
        read_bool("lane_centering", kOnOff, key, value, s.lane_centering);
        read_bool("smooth_traffic", kOnOff, key, value, s.smooth_traffic);
        if (key == "map")
            s.map_name = std::string(value);
        read(kEffects, key, value, s.effects);
        read(kMusic, key, value, s.music);
        read(kGraphics, key, value, s.graphics);
        if (key == "sound") {  // older files: one sound setting
            if (value == "on" || value == "speaker")
                s.effects = Effects::Speaker;
            else if (value == "off")
                s.effects = Effects::Off, s.music = Music::Off;
            else if (value == "adlib")
                s.effects = Effects::AdLib;
            else if (value == "mac")
                s.effects = Effects::Mac;
            else if (value == "pc98")
                s.effects = Effects::AdLib, s.music = Music::Pc98;
        }
        read_bool("launcher", kOnOff, key, value, s.show_launcher);
        if (key == "game_folder")
            s.game_folder = std::string(value);  // everything after the first '=', trimmed
        if (key == "online_address")
            s.online_address = std::string(value);
        read_bool("online_port_forwarded", kOnOff, key, value, s.online_port_forwarded);
        if (key == "online_course" && value.size() == 1 && value[0] >= '1' && value[0] <= '4')
            s.online_course = value[0] - '0';
    }
    return s;
}

Settings load_settings(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in)
        return {};
    std::ostringstream text;
    text << in.rdbuf();
    return Settings::parse(text.str());
}

bool save_settings(const std::filesystem::path& file, const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << settings.serialize();
    return static_cast<bool>(out);
}

}  // namespace vette
