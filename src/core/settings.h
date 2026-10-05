#pragma once
// The player's settings: what the launch menu edits. Stored as `key = value` lines in settings.ini in
// the user's settings folder. Command-line flags override them for one run.

#include <filesystem>
#include <string>

namespace vette {

struct Settings {
    enum class FrameRate { Smooth, Original };
    enum class Pc { Fast, At286 };
    enum class DrawDistance { Original, Extended, Maximum };
    enum class Joystick { Auto, On, Off };
    // Where the sound comes from: the original PC speaker, or a replacement driven by the game's sound
    // events (an emulated AdLib, the PC-98 version's YM2203 FM, the Mac version's digitized sounds).
    enum class Sound { Off, Speaker, AdLib, Pc98, Mac };
    // Whose artwork the screens show: the DOS original's, or the PC-98 or Mac version's.
    enum class Graphics { Dos, Pc98, Mac };
    enum class Preset { Classic, Enhanced, Custom };

    FrameRate frame_rate = FrameRate::Smooth;
    Pc pc = Pc::Fast;
    DrawDistance draw_distance = DrawDistance::Maximum;  // Extended/Maximum: the Enhanced 3D renderer
    bool manual_check = false;  // show the original's copy-protection question
    Joystick joystick = Joystick::Auto;
    bool fullscreen = false;
    Sound sound = Sound::AdLib;
    Graphics graphics = Graphics::Dos;
    bool show_launcher = true;
    std::string game_folder;  // UTF-8; empty: look for Game/ next to the program

    // Classic is VETTE! exactly as shipped in 1989 (its own frames, a 12 MHz PC/AT, the manual
    // question); Enhanced switches every improvement on. Presets set only the options that change
    // how the game looks or plays; preset() reports Custom when the values match neither.
    Preset preset() const;
    void apply(Preset preset);

    std::string serialize() const;
    static Settings parse(const std::string& text);  // unknown keys and bad values are ignored

    bool operator==(const Settings&) const = default;
};

// A missing or unreadable file gives the defaults. save_settings creates the folder if needed.
Settings load_settings(const std::filesystem::path& file);
bool save_settings(const std::filesystem::path& file, const Settings& settings);

}  // namespace vette
