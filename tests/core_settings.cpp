// Settings: presets, and the settings.ini round trip.

#include "core/settings.h"
#include "test.h"

using vette::Settings;

TEST(settings_defaults_are_the_enhanced_preset) {
    const Settings s;
    CHECK(s.preset() == Settings::Preset::Enhanced);
    CHECK(s.show_launcher);
}

TEST(settings_presets) {
    Settings s;
    s.apply(Settings::Preset::Classic);
    CHECK(s.preset() == Settings::Preset::Classic);
    CHECK(s.frame_rate == Settings::FrameRate::Original);
    CHECK(s.pc == Settings::Pc::At286);
    CHECK(s.draw_distance == Settings::DrawDistance::Original);
    CHECK(s.sound == Settings::Sound::Speaker);
    CHECK(s.manual_check);
    s.manual_check = false;  // one change away from a preset is Custom
    CHECK(s.preset() == Settings::Preset::Custom);
    s.apply(Settings::Preset::Enhanced);
    CHECK(s.preset() == Settings::Preset::Enhanced);
    CHECK(s.draw_distance == Settings::DrawDistance::Maximum);
    CHECK(s.sound == Settings::Sound::AdLib);
    // Options outside the presets don't affect which preset is matched.
    s.fullscreen = true;
    s.joystick = Settings::Joystick::Off;
    CHECK(s.preset() == Settings::Preset::Enhanced);
}

TEST(settings_round_trip) {
    Settings s;
    s.apply(Settings::Preset::Classic);
    s.draw_distance = Settings::DrawDistance::Maximum;
    s.joystick = Settings::Joystick::On;
    s.fullscreen = true;
    s.sound = Settings::Sound::Mac;
    s.graphics = Settings::Graphics::Pc98;
    s.show_launcher = false;
    s.game_folder = "C:\\Old Games\\VETTE = 1989 #1";  // spaces, '=' and '#' survive
    CHECK(Settings::parse(s.serialize()) == s);
}

TEST(settings_old_sound_values) {
    CHECK(Settings::parse("sound = on\n").sound == Settings::Sound::Speaker);
    CHECK(Settings::parse("sound = off\n").sound == Settings::Sound::Off);
}

TEST(settings_parse_is_tolerant) {
    const Settings s = Settings::parse("# comment\r\n  pc =  286 \r\nframe_rate = warp9\nnonsense\nunknown = x\n");
    CHECK(s.pc == Settings::Pc::At286);
    CHECK(s.frame_rate == Settings::FrameRate::Smooth);  // bad value: default kept
}
