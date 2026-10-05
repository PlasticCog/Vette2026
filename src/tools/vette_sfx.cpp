// VETTE! 2026 Sound Editor: edits the AdLib sound bank (adlib.ini in the game's settings folder) that
// the game plays its sounds with when Sound is set to AdLib. Every sound has an OPL2 instrument and a
// pitch (the original's own notes, a fixed note or a sweep); the engine has its own voice. Changes are
// heard at once; the game picks up a saved bank while it runs.
//
//   vette_sfx [--bank <file>]

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/game_dir.h"
#include "core/path_utf8.h"
#include "core/settings.h"
#include "game/sound_events.h"
#include "host/machine.h"
#include "platform/audio.h"
#include "platform/presenter.h"
#include "platform/sdl_util.h"
#include "sound/adlib_sfx.h"
#include "sound/sfx_bank.h"
#include "ui/canvas.h"
#include "ui/theme.h"

namespace vette {
namespace {

using namespace ui::theme;
using sound::AdlibSfx;
using sound::OplOperator;
using sound::OplPatch;
using sound::SfxBank;
using sound::SfxVoice;
using sound::SpeakerProgram;

constexpr int kRate = 48000;
constexpr const char* kTitle = "VETTE! 2026 Sound Editor";

// A sound in the list: its bank id and name, and the original's PC-speaker sound when known: notes,
// or for the noise sounds (crashes, gear grind) the speaker clicks.
struct Entry {
    std::string id;
    std::string label;
    std::optional<SpeakerProgram> original;
    std::vector<game::SoundEvents::Pulse> clicks;
};

const char* label(game::Sfx sfx) {
    switch (sfx) {
    case game::Sfx::Engine: return "Engine";
    case game::Sfx::GarageRev: return "Garage rev";
    case game::Sfx::Skid: return "Skid";
    case game::Sfx::Siren: return "Police siren";
    case game::Sfx::TitleTune: return "Title tune";
    case game::Sfx::WinTune: return "Winner's tune";
    case game::Sfx::Crash: return "Crash";
    case game::Sfx::CrashCar: return "Crash into a car";
    case game::Sfx::CrashRail: return "Crash: guard rail";
    case game::Sfx::HitPedestrian: return "Hit a pedestrian";
    case game::Sfx::GearGrind: return "Gear grind";
    case game::Sfx::Horn: return "Horn (X key)";
    case game::Sfx::Helicopter: return "Helicopter view";
    case game::Sfx::CountdownBeep: return "Countdown beep";
    case game::Sfx::CountdownGo: return "Countdown: go";
    case game::Sfx::Splash: return "Into the bay";
    case game::Sfx::Thud: return "Thud: bump or dip";
    case game::Sfx::PulledOver: return "Pulled over";
    case game::Sfx::IntroCableCar: return "Title: cable car";
    case game::Sfx::IntroCar: return "Title: the car";
    case game::Sfx::IntroLogo: return "Title: logo";
    case game::Sfx::Count: break;
    }
    return "";
}

// The original's sounds, read from the player's own VETTE.EXE: it's booted headlessly until it has
// unpacked itself, then every sound's program is read from its memory (game/sound_events.h).
std::vector<Entry> load_originals(std::string& note) {
    std::vector<Entry> entries;
    for (int i = 0; i < static_cast<int>(game::Sfx::Count); ++i) {
        const auto sfx = static_cast<game::Sfx>(i);
        if (sfx != game::Sfx::Engine)
            entries.push_back({game::sfx_name(sfx), label(sfx), std::nullopt, {}});
    }
    char* pref = SDL_GetPrefPath("VETTE2026", "config");
    const Settings settings = pref ? load_settings(path_from_utf8(pref) / "settings.ini") : Settings{};
    SDL_free(pref);
    GameDirSearch search;
    if (!settings.game_folder.empty())
        search = find_game_dir(path_from_utf8(settings.game_folder));
    if (!search.dir)
        search = find_game_dir(std::nullopt);
    if (!search.dir) {
        note = "The DOS game wasn't found, so its original sounds can't be played (O).";
        return entries;
    }
    host::MachineConfig config;
    config.game_dir = search.dir->root();
    config.save_dir = std::filesystem::temp_directory_path() / "vette2026_sfx";  // never the player's saves
    config.audio_rate = kRate;
    config.cpu_hz = 140'000'000;
    host::Machine machine(config);
    std::string error;
    if (!machine.boot(error)) {
        note = "Couldn't start VETTE.EXE (" + error + "), so its original sounds can't be played.";
        return entries;
    }
    game::SoundEvents events(machine);
    machine.run_for(1'500'000'000);  // past the unpacking, well before the title
    for (Entry& e : entries) {
        const game::SoundEvents::Program prog = events.program(*game::sfx_from_name(e.id));
        if (!prog.steps.empty()) {
            SpeakerProgram sp;
            for (const auto& step : prog.steps)
                sp.steps.push_back({step.ticks, step.hz});
            sp.loop_to = prog.loop_to;
            e.original = std::move(sp);
        }
        e.clicks = prog.pulses;
    }
    return entries;
}

// The original sound as the PC speaker played it: a square wave stepping through the program.
class SpeakerPreview {
public:
    void play(const SpeakerProgram& p) {
        pulses_.clear();
        program_ = p;
        step_ = 0;
        left_ = p.steps.empty() ? 0 : p.steps[0].ticks / SpeakerProgram::kTickHz;
        active_ = !p.steps.empty();
    }
    // A noise sound: the speaker cone pushed out for `on` microseconds, then back for `off`, click by click.
    void clicks(const std::vector<game::SoundEvents::Pulse>& pulses) {
        pulses_ = pulses;
        pulse_ = 0;
        pulse_t_ = 0;
        active_ = !pulses.empty();
        program_.steps.clear();
    }
    void tone(float hz) {  // a held note (the engine)
        program_ = SpeakerProgram{{{1, hz}}, 0};
        play(program_);
    }
    void stop() { active_ = false; }
    bool active() const { return active_; }

    void render(float* out, int frames) {
        const double dt = 1.0 / kRate;
        if (!pulses_.empty() && program_.steps.empty()) {
            for (int i = 0; i < frames && active_; ++i) {
                const auto& p = pulses_[pulse_];
                const double us = pulse_t_ * 1e6;
                smooth_ += ((us < p.on_us ? 0.18f : -0.18f) - smooth_) * 0.35f;
                out[i] += smooth_;
                pulse_t_ += dt;
                if (pulse_t_ * 1e6 >= p.on_us + p.off_us) {
                    pulse_t_ = 0;
                    if (++pulse_ >= pulses_.size())
                        active_ = false;
                }
            }
            return;
        }
        for (int i = 0; i < frames && active_; ++i) {
            const float hz = program_.steps[step_].hz;
            float v = 0;
            if (hz > 0) {
                phase_ += hz * dt;
                phase_ -= std::floor(phase_);
                v = phase_ < 0.5 ? 0.18f : -0.18f;
            }
            smooth_ += (v - smooth_) * 0.35f;  // the speaker cone's softening
            out[i] += smooth_;
            left_ -= dt;
            while (left_ <= 0 && active_) {
                if (++step_ >= program_.steps.size()) {
                    if (program_.loop_to < 0) {
                        active_ = false;
                        break;
                    }
                    step_ = static_cast<size_t>(program_.loop_to);
                }
                left_ += std::max(program_.steps[step_].ticks, 1) / SpeakerProgram::kTickHz;
            }
        }
    }

private:
    SpeakerProgram program_;
    std::vector<game::SoundEvents::Pulse> pulses_;
    size_t pulse_ = 0;
    double pulse_t_ = 0;
    size_t step_ = 0;
    double left_ = 0, phase_ = 0;
    float smooth_ = 0;
    bool active_ = false;
};

// One editable value: drawn as label and value; `change(steps)` moves it (steps: +-1, +-10 with Shift).
struct Field {
    std::string label;
    std::string value;
    std::function<void(int)> change;
    int column = 0;  // 0: the sound's settings, 1: modulator, 2: carrier
};

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

const char* yes_no(bool b) { return b ? "Yes" : "No"; }

template <typename T>
void step_int(T& v, int steps, int lo, int hi) {
    v = static_cast<T>(std::clamp(static_cast<int>(v) + steps, lo, hi));
}

void step_semitones(float& hz, int steps, float lo, float hi) {
    hz = std::clamp(static_cast<float>(hz * std::exp2(steps / 12.0)), lo, hi);
}

void add_patch_fields(std::vector<Field>& f, OplPatch& p) {
    f.push_back({"Feedback", std::to_string(p.feedback), [&p](int s) { step_int(p.feedback, s, 0, 7); }});
    f.push_back({"Connection", p.additive ? "Additive (both heard)" : "FM (mod -> car)",
                 [&p](int) { p.additive = !p.additive; }});
    static constexpr const char* kWaves[] = {"Sine", "Half sine", "Abs sine", "Quarter"};
    for (int col = 1; col <= 2; ++col) {
        OplOperator& op = col == 1 ? p.modulator : p.carrier;
        f.push_back({"Wave", kWaves[op.wave & 3], [&op](int s) { op.wave = static_cast<uint8_t>((op.wave + s + 400) % 4); }, col});
        f.push_back({"Multiple", op.multiple == 0 ? "x0.5" : "x" + std::to_string(op.multiple),
                     [&op](int s) { step_int(op.multiple, s, 0, 15); }, col});
        f.push_back({"Level", std::to_string(op.level) + fmt(" (-%.1f dB)", op.level * 0.75),
                     [&op](int s) { step_int(op.level, s, 0, 63); }, col});
        f.push_back({"Attack", std::to_string(op.attack), [&op](int s) { step_int(op.attack, s, 0, 15); }, col});
        f.push_back({"Decay", std::to_string(op.decay), [&op](int s) { step_int(op.decay, s, 0, 15); }, col});
        f.push_back({"Sustain", std::to_string(op.sustain) + fmt(" (-%.0f dB)", op.sustain * 3.0),
                     [&op](int s) { step_int(op.sustain, s, 0, 15); }, col});
        f.push_back({"Release", std::to_string(op.release), [&op](int s) { step_int(op.release, s, 0, 15); }, col});
        f.push_back({"Hold", yes_no(op.sustained), [&op](int) { op.sustained = !op.sustained; }, col});
        f.push_back({"Key scaling", std::to_string(op.scale_level), [&op](int s) { step_int(op.scale_level, s, 0, 3); }, col});
        f.push_back({"Rate scaling", yes_no(op.scale_rate), [&op](int) { op.scale_rate = !op.scale_rate; }, col});
        f.push_back({"Vibrato", yes_no(op.vibrato), [&op](int) { op.vibrato = !op.vibrato; }, col});
        f.push_back({"Tremolo", yes_no(op.tremolo), [&op](int) { op.tremolo = !op.tremolo; }, col});
    }
}

void add_voice_fields(std::vector<Field>& f, SfxVoice& v, bool has_original) {
    static constexpr const char* kPitch[] = {"Original notes", "Fixed note", "Sweep"};
    f.push_back({"Enabled", yes_no(v.enabled), [&v](int) { v.enabled = !v.enabled; }});
    f.push_back({"Pitch", std::string(kPitch[static_cast<int>(v.pitch)]) +
                              (v.pitch == SfxVoice::Pitch::Original && !has_original ? " (unknown here)" : ""),
                 [&v](int s) {
                     v.pitch = static_cast<SfxVoice::Pitch>((static_cast<int>(v.pitch) + (s > 0 ? 1 : 2)) % 3);
                 }});
    f.push_back({"Transpose", fmt("%+.0f semitones", v.transpose),
                 [&v](int s) { v.transpose = std::clamp(v.transpose + static_cast<float>(s), -48.0f, 48.0f); }});
    f.push_back({v.pitch == SfxVoice::Pitch::Sweep ? "From" : "Note", fmt("%.1f Hz", v.hz),
                 [&v](int s) { step_semitones(v.hz, s, 20, 6000); }});
    f.push_back({"To", fmt("%.1f Hz", v.to_hz), [&v](int s) { step_semitones(v.to_hz, s, 20, 6000); }});
    f.push_back({"Length", v.time_ms > 0 ? fmt("%.0f ms", v.time_ms) : "until the game stops it",
                 [&v](int s) { v.time_ms = std::clamp(v.time_ms + 10.0f * static_cast<float>(s), 0.0f, 10000.0f); }});
    f.push_back({"Retrigger", yes_no(v.retrigger), [&v](int) { v.retrigger = !v.retrigger; }});
    f.push_back({"Volume", std::to_string(v.volume) + "%", [&v](int s) { step_int(v.volume, s * 5, 0, 100); }});
    add_patch_fields(f, v.patch);
}

// The game's settings folder, where it reads the bank from.
std::filesystem::path default_bank_path() {
    char* pref = SDL_GetPrefPath("VETTE2026", "config");
    if (!pref)
        return "adlib.ini";
    std::filesystem::path path = path_from_utf8(pref) / "adlib.ini";
    SDL_free(pref);
    return path;
}

class Editor {
public:
    Editor(Presenter& presenter, std::filesystem::path bank_path)
        : presenter_(presenter), bank_path_(std::move(bank_path)), adlib_(kRate) {
        std::string note;
        entries_ = load_originals(note);
        load();
        if (!note.empty())
            status_ = note;
    }

    int run() {
        std::optional<AudioOut> audio;
        try {
            audio.emplace(kRate);
        } catch (const std::exception& e) {
            status_ = std::string("No sound output: ") + e.what();
        }
        std::vector<float> mix;
        std::vector<int16_t> pcm;
        uint64_t last = SDL_GetTicksNS();
        double carry = 0;
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (!handle(e))
                    return 0;
            }
            // Audio for the wall-clock time since the last frame.
            const uint64_t now = SDL_GetTicksNS();
            carry += std::min(0.1, static_cast<double>(now - last) / 1e9) * kRate;
            last = now;
            const int frames = static_cast<int>(carry);
            carry -= frames;
            mix.assign(static_cast<size_t>(frames), 0.0f);
            adlib_.render(mix.data(), frames);
            speaker_.render(mix.data(), frames);
            pcm.resize(mix.size());
            for (size_t i = 0; i < mix.size(); ++i)
                pcm[i] = static_cast<int16_t>(std::clamp(mix[i] * 0.6f, -1.0f, 1.0f) * 32767.0f);
            if (audio)
                audio->push(pcm);
            draw();
            if (!presenter_.visible())
                SDL_Delay(10);
        }
    }

private:
    // The list: 0 = the engine, 1 = the fallback, then the sounds.
    bool engine_selected() const { return selected_ == 0; }
    bool fallback_selected() const { return selected_ == 1; }
    std::string list_label(size_t i) const {
        return i == 0 ? "Engine" : i == 1 ? "(Any other sound)" : entries_[i - 2].label;
    }
    size_t list_size() const { return entries_.size() + 2; }
    const Entry* entry() const { return selected_ >= 2 ? &entries_[selected_ - 2] : nullptr; }
    SfxVoice& voice() {
        if (fallback_selected())
            return bank_.fallback;
        auto it = bank_.sounds.find(entry()->id);
        if (it == bank_.sounds.end())
            it = bank_.sounds.emplace(entry()->id, bank_.fallback).first;
        return it->second;
    }

    void load() {
        bank_ = SfxBank::defaults();
        std::ifstream f(bank_path_, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            bank_ = SfxBank::parse(ss.str());
            status_ = "Loaded " + path_to_utf8(bank_path_);
        } else {
            status_ = "New bank (built-in sounds). S saves it to " + path_to_utf8(bank_path_);
        }
        saved_ = bank_;
        adlib_.set_bank(bank_);
    }

    void save() {
        std::error_code ec;
        std::filesystem::create_directories(bank_path_.parent_path(), ec);
        std::ofstream f(bank_path_, std::ios::binary);
        f << bank_.serialize();
        if (f) {
            saved_ = bank_;
            status_ = "Saved " + path_to_utf8(bank_path_) + ". The game uses it from the next sound on.";
        } else {
            status_ = "Couldn't write " + path_to_utf8(bank_path_);
        }
    }

    std::vector<Field> fields() {
        std::vector<Field> f;
        if (engine_selected()) {
            sound::EngineVoice& e = bank_.engine;
            f.push_back({"Enabled", yes_no(e.enabled), [&e](int) { e.enabled = !e.enabled; }});
            f.push_back({"Pitch", fmt("x%.3f of the original note", e.ratio),
                         [&e](int s) { e.ratio = std::clamp(static_cast<float>(e.ratio * std::exp2(s / 12.0)), 0.0625f, 16.0f); }});
            f.push_back({"Transpose", fmt("%+.0f semitones", e.transpose),
                         [&e](int s) { e.transpose = std::clamp(e.transpose + static_cast<float>(s), -48.0f, 48.0f); }});
            f.push_back({"Volume", std::to_string(e.volume) + "%", [&e](int s) { step_int(e.volume, s * 5, 0, 100); }});
            f.push_back({"Preview note", fmt("%.0f Hz (the original's engine note)", engine_hz_),
                         [this](int s) { step_semitones(engine_hz_, s, 20, 2000); }});
            add_patch_fields(f, e.patch);
        } else {
            add_voice_fields(f, voice(), entry() && entry()->original);
        }
        return f;
    }

    // Plays (or stops) the selected sound with AdLib, or as the original PC speaker played it.
    void play(bool original) {
        adlib_.set_bank(bank_);
        if (engine_selected()) {
            const bool on = !(original ? speaker_.active() : engine_on_);
            adlib_.engine(false, 0);
            speaker_.stop();
            engine_on_ = false;
            if (on && original)
                speaker_.tone(engine_hz_);
            else if (on)
                adlib_.engine(engine_on_ = true, engine_hz_);
            return;
        }
        const std::string id = fallback_selected() ? std::string("fallback") : entry()->id;
        const SpeakerProgram* program = entry() && entry()->original ? &*entry()->original : nullptr;
        if (original) {
            adlib_.stop_all();
            if (speaker_.active())
                speaker_.stop();
            else if (program)
                speaker_.play(*program);
            else if (entry() && !entry()->clicks.empty())
                speaker_.clicks(entry()->clicks);
            else
                status_ = "The original isn't known for this sound.";
            return;
        }
        speaker_.stop();
        if (adlib_.playing(id))
            adlib_.stop(id);
        else
            adlib_.start(id, program);
    }

    void changed() {
        adlib_.set_bank(bank_);
        if (engine_selected() && engine_on_)
            adlib_.engine(true, engine_hz_);
    }

    bool handle(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Scancode k = e.key.scancode;
            const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
            const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
            const int big = shift ? 10 : 1;
            if (k == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
                if (in_fields_) {
                    in_fields_ = false;
                } else if (bank_ != saved_ && !quit_armed_) {
                    status_ = "Unsaved changes: S saves, Esc again quits without saving.";
                    quit_armed_ = true;
                } else {
                    return false;
                }
                break;
            }
            quit_armed_ = false;
            if (k == SDL_SCANCODE_TAB || k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) {
                in_fields_ = !in_fields_ || k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER;
            } else if (k == SDL_SCANCODE_UP || k == SDL_SCANCODE_DOWN) {
                const int dir = k == SDL_SCANCODE_UP ? -1 : 1;
                if (in_fields_) {
                    const int n = static_cast<int>(fields().size());
                    field_ = (field_ + dir + n) % n;
                } else {
                    selected_ = (selected_ + list_size() + static_cast<size_t>(dir)) % list_size();
                    field_ = 0;
                }
            } else if ((k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) && in_fields_) {
                std::vector<Field> f = fields();
                f[static_cast<size_t>(std::min(field_, static_cast<int>(f.size()) - 1))].change(
                    (k == SDL_SCANCODE_LEFT ? -1 : 1) * big);
                changed();
            } else if (k == SDL_SCANCODE_SPACE && !e.key.repeat) {
                play(false);
            } else if (k == SDL_SCANCODE_O && !e.key.repeat) {
                play(true);
            } else if (k == SDL_SCANCODE_S && !e.key.repeat) {
                save();
            } else if (k == SDL_SCANCODE_C && ctrl) {
                clipboard_ = engine_selected() ? bank_.engine.patch : voice().patch;
                status_ = "Instrument copied.";
            } else if (k == SDL_SCANCODE_V && ctrl && clipboard_) {
                (engine_selected() ? bank_.engine.patch : voice().patch) = *clipboard_;
                changed();
                status_ = "Instrument pasted.";
            } else if (k == SDL_SCANCODE_R && ctrl) {
                load();
                status_ = "Reverted to the saved bank.";
            } else if (k == SDL_SCANCODE_F11) {
                presenter_.toggle_fullscreen();
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL: {
            float wx = 0, wy = 0;
            SDL_GetMouseState(&wx, &wy);
            int fx = 0, fy = 0;
            if (!presenter_.window_to_frame(wx, wy, fx, fy))
                break;
            for (const Hit& h : hits_) {
                if (fx >= h.x0 && fx < h.x1 && fy >= h.y0 && fy < h.y1) {
                    if (h.list) {
                        selected_ = static_cast<size_t>(h.index);
                        in_fields_ = false;
                        field_ = 0;
                    } else {
                        in_fields_ = true;
                        field_ = h.index;
                        const int steps = e.type == SDL_EVENT_MOUSE_WHEEL ? (e.wheel.y > 0 ? 1 : -1)
                                          : e.button.button == SDL_BUTTON_RIGHT ? -1
                                          : e.button.clicks >= 2 ? 1 : 0;
                        if (steps) {
                            fields()[static_cast<size_t>(h.index)].change(steps);
                            changed();
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        return true;
    }

    // Mouse targets, from the last draw.
    struct Hit {
        int x0, y0, x1, y1;
        bool list;
        int index;
    };

    void draw() {
        int out_w = 0, out_h = 0;
        presenter_.output_size(out_w, out_h);
        const int scale = std::max(1, std::min(out_w / 620, out_h / 400));
        canvas_.reset(std::max(out_w / scale, 1), std::max(out_h / scale, 1), scale, kBackground);
        hits_.clear();
        const int m = 16, pitch = 12, top = 46;
        canvas_.text(m, 10, "VETTE!", kGold, 2, true);
        canvas_.text(m + ui::text_width("VETTE!", 2) + 16, 10, "Sound Editor", kGold, 2);
        const std::string changed = bank_ != saved_ ? "  (changed)" : "";
        const size_t room = static_cast<size_t>((canvas_.width - 2 * m) / ui::kGlyph) - 22 - changed.size();
        std::string path = path_to_utf8(bank_path_);
        if (path.size() > room && room > 3)
            path = "..." + path.substr(path.size() - (room - 3));
        canvas_.text(m, 30, "AdLib (YM3812) bank: " + path + changed, kSubtitle);

        // The sound list.
        const int list_w = 20 * ui::kGlyph;
        const int list_rows = std::max(1, (canvas_.height - top - 60) / pitch);
        const size_t first = selected_ >= static_cast<size_t>(list_rows) ? selected_ - list_rows + 1 : 0;
        for (size_t i = first; i < list_size() && i < first + static_cast<size_t>(list_rows); ++i) {
            const int y = top + static_cast<int>(i - first) * pitch;
            const bool sel = i == selected_;
            if (sel)
                canvas_.fill_rect(m - 4, y - 3, list_w, pitch, in_fields_ ? kRule : kSelection);
            canvas_.text(m, y, list_label(i), sel ? kGold : i < 2 ? kSubtitle : kLabel);
            hits_.push_back({m - 4, y - 3, m - 4 + list_w, y - 3 + pitch, true, static_cast<int>(i)});
        }

        // The selected sound's settings, then its instrument's operators in two columns.
        const std::vector<Field> f = fields();
        field_ = std::clamp(field_, 0, static_cast<int>(f.size()) - 1);
        const int px = m + list_w + 12;
        const int label_w = 13 * ui::kGlyph;
        const int col_w = (canvas_.width - px - m) / 2;
        int settings_rows = 0;
        for (const Field& fd : f)
            settings_rows += fd.column == 0;
        const int table_y = top + settings_rows * pitch + 8;  // the operator table's heading
        if (settings_rows < static_cast<int>(f.size())) {
            canvas_.text(px, table_y, "Modulator", kSubtitle);
            canvas_.text(px + col_w, table_y, "Carrier", kSubtitle);
            canvas_.fill_rect(px, table_y + 10, canvas_.width - px - m, 1, kRule);
        }
        int in_column[3] = {0, 0, 0};
        for (size_t i = 0; i < f.size(); ++i) {
            const Field& fd = f[i];
            const int r = in_column[fd.column]++;
            const int x = fd.column == 2 ? px + col_w : px;
            const int y = fd.column == 0 ? top + r * pitch : table_y + 16 + r * pitch;
            const int w = fd.column == 0 ? canvas_.width - px - m : col_w - 4;
            const bool sel = in_fields_ && static_cast<int>(i) == field_;
            if (sel)
                canvas_.fill_rect(x - 4, y - 3, w, pitch, kSelection);
            canvas_.text(x, y, fd.label, sel ? kGold : kLabel);
            canvas_.text(x + label_w, y, fd.value, kValue);
            hits_.push_back({x - 4, y - 3, x - 4 + w, y - 3 + pitch, false, static_cast<int>(i)});
        }

        // Status and keys.
        const int hy = canvas_.height - 40;
        canvas_.fill_rect(m, hy - 6, canvas_.width - 2 * m, 1, kRule);
        const size_t line = static_cast<size_t>((canvas_.width - 2 * m) / ui::kGlyph);
        canvas_.text(m, hy, status_.size() > line ? status_.substr(0, line - 3) + "..." : status_, kHelp);
        canvas_.text(m, canvas_.height - 26, "Up/Down choose  Tab list/settings  Left/Right change (Shift: x10)",
                     kHint);
        canvas_.text(m, canvas_.height - 14,
                     "Space play  O original  S save  Ctrl+C/V copy/paste  Ctrl+R revert  Esc quit", kHint);
        presenter_.present(canvas_);
    }

    Presenter& presenter_;
    std::filesystem::path bank_path_;
    AdlibSfx adlib_;
    SpeakerPreview speaker_;
    SfxBank bank_, saved_;
    std::vector<Entry> entries_;
    std::optional<OplPatch> clipboard_;
    ui::Canvas canvas_;
    std::vector<Hit> hits_;
    std::string status_;
    size_t selected_ = 0;
    int field_ = 0;
    bool in_fields_ = false;
    bool quit_armed_ = false;
    bool engine_on_ = false;
    float engine_hz_ = 40;
};

int run(int argc, char** argv) {
    std::optional<std::filesystem::path> bank_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--bank" && i + 1 < argc) {
            bank_arg = path_from_utf8(argv[++i]);
        } else {
            std::fprintf(stderr, "Usage: vette_sfx [--bank <file>]\n");
            return 2;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
        throw_sdl_error("SDL_Init");
    const std::filesystem::path bank = bank_arg.value_or(default_bank_path());
    Presenter presenter(kTitle);
    Editor editor(presenter, bank);
    return editor.run();
}

}  // namespace
}  // namespace vette

int main(int argc, char* argv[]) {
    int code = 1;
    try {
        code = vette::run(argc, argv);
    } catch (const std::exception& e) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, vette::kTitle, e.what(), nullptr);
    }
    SDL_Quit();
    return code;
}
