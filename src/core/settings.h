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
    // The sound effects: the original PC speaker, or a replacement driven by the game's sound events
    // (an emulated AdLib card, the Mac version's digitized sounds).
    enum class Effects { Off, Speaker, AdLib, Mac };
    // The music: the original's tunes (title, winner; played by the effects' device), the PC-98
    // version's FM songs (YM2203: title, menus, winner, loser), or none.
    enum class Music { Off, Original, Pc98 };
    // Whose artwork the screens show: the DOS original's, or the PC-98 or Mac version's.
    enum class Graphics { Dos, Pc98, Mac };
    enum class Preset { Classic, Enhanced, Custom };

    FrameRate frame_rate = FrameRate::Smooth;
    Pc pc = Pc::Fast;
    DrawDistance draw_distance = DrawDistance::Maximum;  // Extended/Maximum: the Enhanced 3D renderer
    bool manual_check = false;  // show the original's copy-protection question
    Joystick joystick = Joystick::Auto;
    bool fullscreen = false;
    // How the 2D pictures are enlarged: Sharp keeps every original pixel a solid block (nearest
    // neighbour); Smooth softens the edges between them.
    enum class Scaling { Sharp, Smooth };
    Scaling scaling = Scaling::Sharp;
    // The painted backdrop behind the Enhanced 3D view (Extended and Maximum draw distance): Hills keeps
    // only the landscape, as the real city stands in front of it; Painted is the original's, with its
    // painted skyline and bridges.
    enum class Skyline { Hills, Painted };
    Skyline skyline = Skyline::Hills;
    // The resolution the Enhanced 3D view (Extended and Maximum draw distance) is drawn at: the
    // display's, or the original's 320x200, then enlarged like the rest of the picture.
    enum class ViewResolution { Display, Original };
    ViewResolution view_resolution = ViewResolution::Display;
    // The Enhanced 3D view with a depth buffer: nearer things always cover farther ones, and the whole
    // city's traffic and pedestrians are drawn. Off (or without a GPU that can): the original's
    // back-to-front order, with traffic and pedestrians only near the car, where the original has them.
    bool depth_buffer = true;
    // Driving (they change how the race plays, so they're off unless chosen; Classic turns them off).
    // Improved: the player's car drifts a little through fast corners and leaves the ground over crests
    // at speed. Lane centering: a slight steering assist toward the lane's direction and centre.
    bool improved_driving = false;
    bool lane_centering = false;
    Effects effects = Effects::AdLib;
    Music music = Music::Original;
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
