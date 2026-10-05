#include "sound/pc98_cues.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "game/x86.h"
#include "host/machine.h"

namespace vette::sound {
namespace {

constexpr uint16_t kCode = game::emu_seg(0x3009);

// DOS code offsets parallel to the PC-98's driver calls (PC-98 addresses: image segment 0000), with the
// instruction bytes expected there (none with a relocation: those differ in memory).
struct Cue {
    uint16_t offset;
    std::optional<Pc98Song> song;
    std::array<uint8_t, 4> code;  // a 3-byte call is followed by a don't-care 00h
};
const Cue kCues[] = {
    {0xC560, Pc98Song::Title, {0xE8, 0x0D, 0xCF, 0}},   // call title tune      (PC-98 0000:BCB7 play 1)
    {0xC61B, std::nullopt, {0xE8, 0x34, 0xCD, 0}},      // call snd_stop        (PC-98 0000:BD77 stop)
    {0x0071, Pc98Song::Menu, {0xE8, 0xAE, 0x89, 0}},    // call the menu        (PC-98 0000:003F play 4)
    {0x00B7, std::nullopt, {0x8E, 0xD8, 0x8E, 0xC0}},  // on to the race       (PC-98 0000:007E stop)
    {0x062F, std::nullopt, {0xE8, 0x40, 0xEF, 0}},      // quit                 (PC-98 0000:050D driver off)
    {0xCA10, Pc98Song::Loser, {0xE8, 0x0E, 0xA6, 0}},   // loser picture shown  (PC-98 0000:BFF3 play 3)
    {0xCA27, std::nullopt, {0x80, 0x3E, 0xFF, 0x2A}},  // key pressed        (PC-98 0000:C012 stop)
    {0xCEBD, Pc98Song::Winner, {0xE8, 0xB8, 0xC6, 0}},  // call winner tune     (PC-98 0000:C3F1 play 2)
    {0xCEC3, std::nullopt, {0xE8, 0x8C, 0xC4, 0}},      // call snd_stop        (PC-98 0000:C3FC stop)
};

}  // namespace

Pc98MusicCues::Pc98MusicCues(host::Machine& machine) : machine_(machine) {
    host::Cpu& cpu = machine.cpu();
    for (const Cue& cue : kCues) {
        watches_.push_back(cpu.add_watch(host::Cpu::linear(kCode, cue.offset), [this, &cue](host::Cpu&) {
            const size_t n = cue.code[3] ? 4 : 3;
            for (size_t i = 0; i < n; ++i) {
                if (game::rd8(machine_.memory(), kCode, static_cast<uint16_t>(cue.offset + i)) != cue.code[i]) {
                    return;
                }
            }
            if (cues_.size() < 1024) {
                cues_.push_back({machine_.emulated_ns(), cue.song});
            }
        }));
    }
}

Pc98MusicCues::~Pc98MusicCues() {
    for (const host::Cpu::WatchId id : watches_) {
        machine_.cpu().remove_watch(id);
    }
}

void Pc98MusicCues::take(std::vector<Pc98Cue>& out) {
    out.insert(out.end(), cues_.begin(), cues_.end());
    cues_.clear();
}

void Pc98MusicCues::apply(Pc98Sound& sound) {
    for (const Pc98Cue& c : cues_) {
        if (c.song) {
            sound.play(*c.song);
        } else if (sound.playing()) {
            sound.stop();
        }
    }
    cues_.clear();
}

}  // namespace vette::sound
