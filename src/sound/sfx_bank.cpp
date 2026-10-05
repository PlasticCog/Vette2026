#include "sound/sfx_bank.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <sstream>

namespace vette::sound {
namespace {

constexpr std::string_view kEngine = "engine", kFallback = "fallback";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool parse_bool(std::string_view v, bool& out) {
    if (v == "yes" || v == "on" || v == "true" || v == "1") {
        out = true;
        return true;
    }
    if (v == "no" || v == "off" || v == "false" || v == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parse_number(std::string_view v, double lo, double hi, double& out) {
    const std::string s(v);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(d))
        return false;
    out = std::clamp(d, lo, hi);
    return true;
}

template <typename T>
void read_int(std::string_view v, int lo, int hi, T& out) {
    double d = 0;
    if (parse_number(v, lo, hi, d))
        out = static_cast<T>(std::lround(d));
}

void read_float(std::string_view v, double lo, double hi, float& out) {
    double d = 0;
    if (parse_number(v, lo, hi, d))
        out = static_cast<float>(d);
}

void read_bool(std::string_view v, bool& out) {
    bool b = false;
    if (parse_bool(v, b))
        out = b;
}

// Operator keys: "wave", "multiple", ...; true if `key` was one.
bool read_operator(std::string_view key, std::string_view v, OplOperator& op) {
    if (key == "wave") read_int(v, 0, 3, op.wave);
    else if (key == "multiple") read_int(v, 0, 15, op.multiple);
    else if (key == "level") read_int(v, 0, 63, op.level);
    else if (key == "attack") read_int(v, 0, 15, op.attack);
    else if (key == "decay") read_int(v, 0, 15, op.decay);
    else if (key == "sustain") read_int(v, 0, 15, op.sustain);
    else if (key == "release") read_int(v, 0, 15, op.release);
    else if (key == "scale_level") read_int(v, 0, 3, op.scale_level);
    else if (key == "sustained") read_bool(v, op.sustained);
    else if (key == "vibrato") read_bool(v, op.vibrato);
    else if (key == "tremolo") read_bool(v, op.tremolo);
    else if (key == "scale_rate") read_bool(v, op.scale_rate);
    else return false;
    return true;
}

bool read_patch(std::string_view key, std::string_view v, OplPatch& p) {
    if (key.starts_with("mod.")) return read_operator(key.substr(4), v, p.modulator);
    if (key.starts_with("car.")) return read_operator(key.substr(4), v, p.carrier);
    if (key == "feedback") read_int(v, 0, 7, p.feedback);
    else if (key == "additive") read_bool(v, p.additive);
    else return false;
    return true;
}

void read_voice(std::string_view key, std::string_view v, SfxVoice& s) {
    if (read_patch(key, v, s.patch)) return;
    if (key == "enabled") read_bool(v, s.enabled);
    else if (key == "pitch") {
        if (v == "original") s.pitch = SfxVoice::Pitch::Original;
        else if (v == "fixed") s.pitch = SfxVoice::Pitch::Fixed;
        else if (v == "sweep") s.pitch = SfxVoice::Pitch::Sweep;
    } else if (key == "transpose") read_float(v, -48, 48, s.transpose);
    else if (key == "hz") read_float(v, 1, 6000, s.hz);
    else if (key == "to_hz") read_float(v, 1, 6000, s.to_hz);
    else if (key == "time_ms") read_float(v, 0, 60000, s.time_ms);
    else if (key == "retrigger") read_bool(v, s.retrigger);
    else if (key == "volume") read_int(v, 0, 100, s.volume);
}

void read_engine(std::string_view key, std::string_view v, EngineVoice& e) {
    if (read_patch(key, v, e.patch)) return;
    if (key == "enabled") read_bool(v, e.enabled);
    else if (key == "ratio") read_float(v, 0.0625, 16, e.ratio);
    else if (key == "transpose") read_float(v, -48, 48, e.transpose);
    else if (key == "volume") read_int(v, 0, 100, e.volume);
}

const char* yes(bool b) { return b ? "yes" : "no"; }

void write_patch(std::ostream& out, const OplPatch& p) {
    out << "feedback = " << int{p.feedback} << "\nadditive = " << yes(p.additive) << "\n";
    for (const auto& [prefix, op] : {std::pair{"mod.", &p.modulator}, std::pair{"car.", &p.carrier}}) {
        out << prefix << "wave = " << int{op->wave} << "\n"
            << prefix << "multiple = " << int{op->multiple} << "\n"
            << prefix << "level = " << int{op->level} << "\n"
            << prefix << "attack = " << int{op->attack} << "\n"
            << prefix << "decay = " << int{op->decay} << "\n"
            << prefix << "sustain = " << int{op->sustain} << "\n"
            << prefix << "release = " << int{op->release} << "\n"
            << prefix << "scale_level = " << int{op->scale_level} << "\n"
            << prefix << "sustained = " << yes(op->sustained) << "\n"
            << prefix << "vibrato = " << yes(op->vibrato) << "\n"
            << prefix << "tremolo = " << yes(op->tremolo) << "\n"
            << prefix << "scale_rate = " << yes(op->scale_rate) << "\n";
    }
}

void write_voice(std::ostream& out, std::string_view name, const SfxVoice& s) {
    static constexpr const char* kPitch[] = {"original", "fixed", "sweep"};
    out << "\n[" << name << "]\n"
        << "enabled = " << yes(s.enabled) << "\n"
        << "pitch = " << kPitch[static_cast<int>(s.pitch)] << "\n"
        << "transpose = " << s.transpose << "\n"
        << "hz = " << s.hz << "\n"
        << "to_hz = " << s.to_hz << "\n"
        << "time_ms = " << s.time_ms << "\n"
        << "retrigger = " << yes(s.retrigger) << "\n"
        << "volume = " << s.volume << "\n";
    write_patch(out, s.patch);
}

}  // namespace

int volume_to_level(int percent) {
    if (percent <= 0)
        return 63;
    const double db = -20.0 * std::log10(std::min(percent, 100) / 100.0);
    return std::clamp(static_cast<int>(std::lround(db / 0.75)), 0, 63);
}

const SfxVoice& SfxBank::sound(std::string_view name) const {
    const auto it = sounds.find(name);
    return it != sounds.end() ? it->second : fallback;
}

SfxBank SfxBank::defaults() {
    SfxBank bank;
    // Fallback: a bright, slightly buzzy lead, close in spirit to the speaker's square wave.
    OplPatch& f = bank.fallback.patch;
    f.feedback = 6;
    f.modulator = {.multiple = 1, .level = 22, .attack = 15, .decay = 0, .sustain = 0, .release = 7};
    f.carrier = {.multiple = 1, .level = 0, .attack = 15, .decay = 0, .sustain = 0, .release = 7};
    // Engine: a low, rough hum; a second harmonic modulator with heavy feedback gives the grit.
    OplPatch& e = bank.engine.patch;
    e.feedback = 7;
    e.modulator = {.multiple = 2, .level = 18, .attack = 12, .decay = 0, .sustain = 0, .release = 8};
    e.carrier = {.multiple = 1, .level = 0, .attack = 12, .decay = 0, .sustain = 0, .release = 8};
    return bank;
}

std::string SfxBank::serialize() const {
    std::ostringstream out;
    out << "# VETTE! 2026 AdLib sound bank: edit it with vette_sfx (or by hand).\n"
           "# Sections: [engine], [fallback] (any sound not listed), then one per sound.\n"
           "\n[" << kEngine << "]\n"
        << "enabled = " << yes(engine.enabled) << "\nratio = " << engine.ratio << "\ntranspose = " << engine.transpose
        << "\nvolume = " << engine.volume << "\n";
    write_patch(out, engine.patch);
    write_voice(out, kFallback, fallback);
    for (const auto& [name, voice] : sounds)
        write_voice(out, name, voice);
    return out.str();
}

SfxBank SfxBank::parse(std::string_view text) {
    SfxBank bank = defaults();
    std::string section;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(trim(line.substr(1, line.size() - 2)));
            if (!section.empty() && section != kEngine && section != kFallback && !bank.sounds.contains(section))
                bank.sounds.emplace(section, bank.fallback);  // a new sound starts from the fallback as read so far
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || section.empty())
            continue;
        const std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == kEngine)
            read_engine(key, value, bank.engine);
        else if (section == kFallback)
            read_voice(key, value, bank.fallback);
        else
            read_voice(key, value, bank.sounds.find(section)->second);
    }
    return bank;
}

}  // namespace vette::sound
