#include "assets/mac_sounds.h"

#include <algorithm>
#include <cmath>

namespace vette::assets {

namespace {

// Big-endian reads (the caller checks the bounds).
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

constexpr double kHalfTick = 1.0 / (2 * kMacTickHz);  // the driver's duration unit (it counts twice a frame)

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

std::int16_t from_u8(std::uint8_t b) { return static_cast<std::int16_t>((b - 128) * 256); }

// From the call sites in Color VETTE! 1.02 (segment:offset of the BogasPlay call). Rates are 11127 Hz
// times the step; durations count the driver's half ticks.
constexpr double kStep1 = kMacMixHz, kStep2 = 2 * kMacMixHz;
const MacSoundUse kUses[] = {
    {"engine", "car-select screen: a click on the chosen car revs it until its animation ends (2:0DD4-0F96)", 0,
     kStep1, 0x708 * kHalfTick,
     "garage_rev"},
    {"engine", "race, while the engine sound is on: looped, pitched by the revs each frame (6:375E)", 0,
     kMacMixHz * 27000 / 65536, 0, "engine"},
    {"engine", "intro, 10 s in: the car drives off (8:0564)", 0, kStep1, 0x168 * kHalfTick, nullptr},
    {"heli", "helicopter view (key 4): replaces the engine, looped (1:2F56)", 0, kStep2, 0, "helicopter"},
    {"horn", "horn key held: looped until it is released (1:30E4)", 1, kStep2, 0, "horn"},
    {"police", "police car within 2048 units: siren looped until it falls back or pulls you over (6:0EB0)", 1,
     kStep1, 0, "siren"},
    {"joel", "pulled over by the police: a voice (only with 1.5 MB free; 6:0F68)", 1, kStep1, 0x12C * kHalfTick,
     "pulled_over"},
    {"skid", "sliding (slip of 250 or more), at most every 2 s (6:3922)", 1, kStep1, 0xF0 * kHalfTick, "skid"},
    {"crash", "hit a building or wall at speed 15 or more, at most every 2 s (6:4720)", 2, kStep1,
     0xF0 * kHalfTick, "crash"},
    {"crash", "hit a car (traffic, police or the opponent): the cars bounce apart (6:02A0)", 2, kStep1,
     0x78 * kHalfTick, "crash_car"},
    {"crash", "the guard rails are walls on the Mac (6:4720)", 2, kStep1, 0xF0 * kHalfTick, "crash_rail"},
    {"crash", "wrecked: a hit hard enough, or one damage too many, ends the race; all channels cut first (6:4DA6)",
     2, kStep1, 0x78 * kHalfTick, nullptr},
    {"kill", "ran into a pedestrian, who is knocked over (1:3F5A)", 1, kStep1, 0x78 * kHalfTick, "hit_pedestrian"},
    {"thud", "over a kerb or edge, either way (6:415A)", 2, kStep1, 0x78 * kHalfTick, "thud"},
    {"thud", "landing hard after a jump, with a 0.5 s skid (6:4370)", 2, kStep1, 0x78 * kHalfTick, nullptr},
    {"cable car bell",
     "onto a service station's driveway, the box where stopping repairs the car; again after leaving it (6:56B2)",
     2, kStep1, 0x168 * kHalfTick, "service_station"},
    {"cable car bell", "intro, 3 s in (8:0496)", 1, kStep1, 0xF0 * kHalfTick, "intro_cable_car"},
    {"beep1", "race start countdown: first and second beep, 2.3 s apart (1:26EC)", 2, kStep1, 0x96 * kHalfTick,
     "countdown_beep"},
    {"beep2", "race start countdown: the third, higher beep (1:26EC)", 2, kStep1, 0x96 * kHalfTick,
     "countdown_go"},
    {"splash", "drove into the bay (only with 1.5 MB free; 6:5B02)", 0, kStep1, 0x168 * kHalfTick, "splash"},
    {"opening song", "intro (title) start, once (8:02F2)", 2, kStep1, 0x528 * kHalfTick, "title_tune"},
    {"mic", "intro: the car passes mid-screen; cuts the opening song off (8:0858)", 2, kStep1, 0x168 * kHalfTick,
     "intro_car"},
    {"signature", "intro, 16 s in: the closing sound (8:09A0)", 0, kStep1, 0x1E0 * kHalfTick, "intro_logo"},
};

}  // namespace

std::optional<MacSound> decode_inst(std::span<const std::uint8_t> inst, std::int16_t id, std::string_view name,
                                    std::string* error) {
    if (inst.size() < 8) {
        if (error) *error = "INST shorter than its header";
        return std::nullopt;
    }
    if (inst[0] == 'H' && inst[1] == 'C' && inst[2] == 'O' && inst[3] == 'M') {
        if (error) *error = "HCOM-compressed INST (not supported)";
        return std::nullopt;
    }
    MacSound s;
    s.type = fourcc("INST");
    s.id = id;
    s.name = std::string(name);
    const std::size_t len = inst.size() - 8;
    const std::size_t loop_start = be16(inst.data()), loop_end = be16(inst.data() + 2);
    const int note = inst[4], flags = inst[5];
    const std::size_t count = be16(inst.data() + 6);
    // Four of VETTE!'s INSTs were stored without a header; their first 8 bytes are samples near 0x80.
    s.header_valid = flags == 0 && note <= 96 && count <= len &&
                     ((loop_start == 0 && loop_end == 0) || (loop_start < loop_end && loop_end <= len));
    std::size_t end = len;
    if (s.header_valid) {
        s.base_note = note ? note : 37;  // the driver's default
        s.native_rate = kMacMixHz * std::pow(2.0, (37 - s.base_note) / 12.0);
        // The driver loops when the loop start is nonzero; otherwise it stops at the count.
        if (loop_start != 0) {
            s.loop_start = loop_start;
            s.loop_end = loop_end;
        } else if (count != 0) {
            end = count;
        }
    }
    s.rate = s.native_rate;
    s.samples.reserve(end);
    for (std::size_t i = 0; i < end; ++i) s.samples.push_back(from_u8(inst[8 + i]));
    return s;
}

std::optional<MacSound> decode_snd(std::span<const std::uint8_t> snd, std::int16_t id, std::string_view name,
                                   std::string* error) {
    auto fail = [&](const char* why) -> std::optional<MacSound> {
        if (error) *error = why;
        return std::nullopt;
    };
    const std::uint8_t* p = snd.data();
    const std::size_t n = snd.size();
    if (n < 6) return fail("'snd ' too short");
    std::size_t cmds = 0;
    const std::uint16_t format = be16(p);
    if (format == 1) {
        cmds = 4 + 6u * be16(p + 2);  // after the modifier list
    } else if (format == 2) {
        cmds = 4;
    } else {
        return fail("'snd ' of an unknown format");
    }
    if (cmds + 2 > n) return fail("'snd ' truncated");
    const std::size_t count = be16(p + cmds);
    std::size_t header = 0;
    for (std::size_t i = 0; i < count && cmds + 2 + 8 * (i + 1) <= n; ++i) {
        const std::uint8_t* c = p + cmds + 2 + 8 * i;
        const std::uint16_t cmd = be16(c);
        if (cmd == 0x8050 || cmd == 0x8051) {  // soundCmd / bufferCmd with the header in this resource
            header = be32(c + 4);
            break;
        }
    }
    if (header == 0 || header + 22 > n) return fail("'snd ' has no sampled sound");
    const std::uint8_t* h = p + header;
    MacSound s;
    s.type = fourcc("snd ");
    s.id = id;
    s.name = std::string(name);
    s.native_rate = s.rate = be32(h + 8) / 65536.0;
    std::size_t loop_start = be32(h + 12), loop_end = be32(h + 16);
    s.base_note = h[21] ? h[21] : 60;
    const std::uint8_t encode = h[20];
    std::size_t frames = 0, channels = 1, bytes = 1, data = header + 22;
    if (encode == 0x00) {
        frames = be32(h + 4);
    } else if (encode == 0xFF) {
        if (header + 64 > n) return fail("'snd ' truncated");
        channels = std::max<std::size_t>(1, be32(h + 4));
        frames = be32(h + 22);
        bytes = be16(h + 48) == 16 ? 2 : 1;
        data = header + 64;
    } else {
        return fail("compressed 'snd ' (not supported)");
    }
    if (be32(h) != 0 || channels > 8) return fail("'snd ' data not in the resource");
    frames = std::min(frames, (n - data) / (channels * bytes));
    s.bits = static_cast<int>(bytes * 8);
    s.samples.reserve(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        int sum = 0;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const std::uint8_t* x = p + data + (f * channels + ch) * bytes;
            sum += bytes == 2 ? static_cast<std::int16_t>(be16(x)) : from_u8(*x);
        }
        s.samples.push_back(static_cast<std::int16_t>(sum / static_cast<int>(channels)));
    }
    if (loop_end > loop_start + 1 && loop_end <= frames) {
        s.loop_start = loop_start;
        s.loop_end = loop_end;
    }
    return s;
}

std::vector<MacSound> decode_mac_sounds(const ResourceFork& fork, std::vector<std::string>* problems) {
    std::vector<MacSound> out;
    for (const Resource& r : fork.resources()) {
        const bool inst = r.type == fourcc("INST");
        if (!inst && r.type != fourcc("snd ")) continue;
        std::string error;
        auto s = inst ? decode_inst(fork.data(r), r.id, r.name, &error)
                      : decode_snd(fork.data(r), r.id, r.name, &error);
        if (!s) {
            if (problems)
                problems->push_back(fourcc_string(r.type) + " " + std::to_string(r.id) + " '" + r.name + "': " + error);
            continue;
        }
        if (inst) {
            if (const MacSoundUse* use = mac_sound_use(s->name)) s->rate = use->rate;
        }
        out.push_back(std::move(*s));
    }
    return out;
}

std::optional<ResourceFork> find_vette_data(const MacFiles& files) {
    if (auto fork = files.resources("VETTE!.Data"); fork && !fork->of_type(fourcc("INST")).empty()) return fork;
    if (const MacFile* f = files.find_type(fourcc("DATA"), fourcc("VETT"))) {
        if (auto fork = files.resources(*f); fork && !fork->of_type(fourcc("INST")).empty()) return fork;
    }
    for (const MacFile& f : files.files()) {
        if (f.rsrc_size == 0) continue;
        if (auto fork = files.resources(f); fork && !fork->of_type(fourcc("INST")).empty()) return fork;
    }
    return std::nullopt;
}

std::span<const MacSoundUse> mac_sound_uses() { return kUses; }

const MacSoundUse* mac_sound_use(std::string_view sound) {
    for (const MacSoundUse& u : kUses) {
        if (ieq(u.sound, sound)) return &u;
    }
    return nullptr;
}

const MacSoundUse* mac_sound_for_dos(std::string_view dos_sfx) {
    for (const MacSoundUse& u : kUses) {
        if (u.dos_sfx && dos_sfx == u.dos_sfx) return &u;
    }
    return nullptr;
}

double mac_engine_rate(double rpm) {
    const double step = rpm <= 1200 ? 27000 : std::min(85000.0, 15000 + 10 * rpm);
    return kMacMixHz * step / 65536;
}

}  // namespace vette::assets
