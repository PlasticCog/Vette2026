#pragma once
// The DOS game's sounds as events, for replacement sound (AdLib FM, the PC-98's YM2203, the Mac's
// samples). An observer: watches on the original's PC-speaker driver and its callers report each sound
// at the moment the original starts or stops it. Nothing the game does is changed.
//
// The original has one voice (re/notes/07-sound.md). Its driver plays one PIT channel 2 program at a
// time (engine, skid, siren, tunes), chosen each race frame by priority (skid > siren > engine). Events
// follow the speaker: a skid cuts the engine off (engine stop, skid start), and the engine starts again
// when the skid ends. requested() tells what the race asks for whatever the priority, for a mixer that
// can play several sounds at once.
//
// Two noise routines (the crashes, the gear grind) click the speaker directly in a CPU-timed loop that
// freezes the game. They don't stop the tone program: only hitting a city car or knocking a pedestrian
// down stops it first, so the other noises' events overlap the engine (or skid, or siren) events.
// Their length, and the garage rev's, scale with the emulated CPU (a crash is 0.12 s at 12 MHz, 0.01 s
// at 140 MHz); replacement sounds should keep their own length.

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "host/machine.h"

namespace vette::game {

// One per sound the DOS game makes, by meaning. Several meanings can share one DOS sound (the crash
// noise); the names are stable ids for files (sfx_name).
enum class Sfx : uint8_t {
    Engine,         // race engine note; its pitch slides as the revs change (SoundEvents::engine())
    GarageRev,      // car-select screen, Space: the engine revved while the exhaust animation runs
    Skid,           // tyre squeal while the car slides
    Siren,          // police car chasing, close behind
    TitleTune,      // title screen melody (plays once)
    WinTune,        // results screen fanfare after finishing ahead of the opponent
    Crash,          // ran into something solid: a building, wall or object, or the opponent's car
    CrashCar,       // hit a traffic car or the police patrol car (city or highway)
    CrashRail,      // hit the highway's guard rail
    HitPedestrian,  // ran into a pedestrian (standing or already knocked down)
    GearGrind,      // missed shift: a gear too low for the speed, or reverse while moving
    Count
};

const char* sfx_name(Sfx);  // "engine", "skid", "crash_car", ...; nullptr for Count
std::optional<Sfx> sfx_from_name(std::string_view);

struct SoundEvent {
    uint64_t t_ns;  // emulated time (Machine::emulated_ns)
    Sfx sfx;
    bool start;  // false: it ended, or the game cut it off
};

struct EngineSound {
    bool on = false;       // the PC speaker plays the engine note now
    bool running = false;  // a race is on with the engine sound enabled (`on` unless a higher-priority
                           // sound has the speaker)
    float speaker_hz = 0;  // the note the speaker plays (PIT channel 2's latch), 0 when not `on`
    float pitch_hz = 0;    // the driver's sliding pitch (cs:92A8), updated at 72.8 Hz; the speaker
                           // takes it every 2-3 ticks
    int slide = 0;         // +1 pitch rising (the revs went up), -1 falling, 0 holding; re-evaluated
                           // only on race frames where the engine has the speaker
    float rpm = 0;         // the revs (engine_speed x 100)
    float idle_rpm = 0, redline_rpm = 0;  // of the player's car model
    bool throttle = false;                // accelerator held this frame
    int gear = 0;                         // 0 neutral, 1..6, -1 reverse
};

// PIT input clock, and the driver's sequence step rate (PIT channel 0 at divisor 1000h, every 4th IRQ).
inline constexpr double kPitHz = 1193181.8;
inline constexpr double kSoundTickHz = kPitHz / 0x4000;  // 72.83 Hz, 13.73 ms

class SoundEvents {
public:
    explicit SoundEvents(host::Machine& machine);
    ~SoundEvents();
    SoundEvents(const SoundEvents&) = delete;
    SoundEvents& operator=(const SoundEvents&) = delete;

    // Appends the events since the last call, in order. Unclaimed events beyond a few thousand are
    // dropped, oldest first.
    void take(std::vector<SoundEvent>& out);

    EngineSound engine() const;  // the engine note right now
    bool enabled() const;        // the game's own sound setting (S key; kept while its options menu
                                 // mutes the speaker)
    // Engine, Skid, Siren: true while a race runs, the sound is on and the race asks for the sound,
    // whether or not it has the speaker. The other sounds: true while they play.
    bool requested(Sfx) const;
    std::optional<Sfx> playing() const;  // the speaker program (tone) sound playing now

    // The original's speaker program for a sound, read from the running game's memory (never stored
    // in the repo): for the sound editor's "original" preview. Empty until the game is unpacked.
    struct Step {
        uint16_t ticks;  // at kSoundTickHz (a loop's jump entry takes a tick too: counted in the last step)
        float hz;        // 0 = rest
    };
    struct Pulse {
        float on_us, off_us;  // one click: speaker cone out, then back
    };
    struct Program {
        std::vector<Step> steps;    // tone sounds: a square wave per step (repeated notes run together:
                                    // the speaker doesn't stop between entries)
        int loop_to = -1;           // after the last step, continue at this step (-1: the sound ends)
        std::vector<Pulse> pulses;  // noise sounds (crashes, gear grind): the clicks, timed for the
                                    // emulated CPU; the spacing is random in the original
    };
    Program program(Sfx) const;

private:
    void on_play();
    void on_noise(bool start, bool grind);
    void on_frame();
    void set_playing(std::optional<Sfx> sfx);
    void push(Sfx sfx, bool start);
    bool race_running() const;
    bool game_loaded() const;

    host::Machine& machine_;
    std::vector<host::Cpu::WatchId> watches_;
    std::vector<SoundEvent> events_;
    std::optional<Sfx> playing_;  // the tone program
    std::optional<Sfx> noise_;    // the noise routine running now
    uint16_t latched_ = 0;        // the divisor last written to PIT channel 2
    bool in_menu_ = false;        // the options menu has muted the game and saved its setting
    // At the last race frame (the request dispatcher 3009:94FC, once per frame loop pass).
    uint64_t frame_ns_ = 0;
    bool frame_seen_ = false;
    bool throttle_ = false;
    int gear_ = 0;
};

// --- Decoding and models of the original's driver (also used by tests) ------------------------------

// PIT divisor to Hz (0 = 65536).
float divisor_hz(uint16_t divisor);

// Decodes a driver sequence starting at code offset `start`: {w ticks, w divisor} entries, divisor
// FFFFh = rest, ticks 0 = jump back by `divisor` bytes (a loop), ticks FFFFh = end. `read16` reads a
// word of the code segment. Stops after 256 entries if neither ending is found.
SoundEvents::Program decode_sequence(const std::function<uint16_t(uint16_t)>& read16, uint16_t start);

// The noise routines (3009:9498 crash, 3009:94C9 gear grind): `clicks` pulses; each is on for a random
// 0..mask LOOP count (0 counts as 65536), then off for `off_start + n * off_step` (crash, n = 1..clicks)
// or, with `off_from_on`, the on count plus `off_step` (grind). The original's randomness is a PIT
// counter read; this uses a fixed pseudo-random sequence from `seed`.
struct NoiseParams {
    int clicks = 0;
    uint16_t mask = 0;
    uint16_t off_start = 0, off_step = 0;
    bool off_from_on = false;
};
std::vector<SoundEvents::Pulse> noise_pulses(const NoiseParams& p, double cpu_hz, uint32_t seed = 1);

// The engine note's pitch slide, exactly as the driver computes it:
// once per race frame, snd_engine_set_target (3009:93EA) picks the slide word from the revs
// (engine_speed, 100 rpm units) and the previous frame's revs...
uint16_t engine_slide(uint16_t engine_speed, uint16_t last_speed, uint16_t slide);
// ...and at 72.8 Hz snd_engine_pitch_tick (3009:9428) moves the divisor by it.
uint16_t engine_pitch_tick(uint16_t divisor, uint16_t slide, uint16_t engine_speed);

} // namespace vette::game
