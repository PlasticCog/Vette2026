#include "game/sound_events.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iterator>

#include "game/x86.h"

namespace vette::game {
namespace {

using host::Cpu;

constexpr uint16_t kCode = emu_seg(0x3009);
constexpr uint16_t kData = kDataSeg;

// The PC-speaker driver and its callers (re/notes/07-sound.md). Watch points:
constexpr uint16_t kPlayCommit = 0x9347;   // snd_play past its sound_enabled test: AX = sequence
constexpr uint16_t kStop = 0x9352;         // snd_stop
constexpr uint16_t kSetDivisor = 0x9360;   // spk_set_divisor: CX = divisor
constexpr uint16_t kSeqEnd = 0x937E;       // snd_seq_tick at an end entry (every tick while idle)
constexpr uint16_t kCrashNoise = 0x94A1;   // snd_noise_crash past its sound_enabled test
constexpr uint16_t kCrashNoiseEnd = 0x94C8;
constexpr uint16_t kGrindNoise = 0x94D2;   // snd_noise_grind past its sound_enabled test
constexpr uint16_t kGrindNoiseEnd = 0x94FB;
constexpr uint16_t kDispatch = 0x94FC;     // snd_request_dispatch: once per race frame
constexpr uint16_t kMenuMute = 0xE6B0;     // options menu: setting saved in cs:E5FC, then muted
constexpr uint16_t kMenuRestore = 0xEB0C;  // options menu: setting restored

// The silent moments (re/notes/07-sound.md, section 9).
constexpr uint16_t kCountdown = 0xBB00;      // start_countdown_step, every frame until the lights go out
constexpr uint16_t kIntoTheBay = 0x198A;     // collision_box_event: the water box (C606h)
constexpr uint16_t kPulledOver = 0x1562;     // police_step: `mov [2C59],1`, the car stopped by the police
constexpr uint16_t kTitleReveal = 0xC55D;    // title_screen: the picture dissolves in
constexpr uint16_t kTitleCableCar = 0xC5A6;  // title_screen: one step of the cable car rolling in
constexpr uint16_t kTitleCar = 0xC612;       // title_screen: the Corvette coming at you (title_car_approach)
constexpr uint16_t kTitleLogo = 0xC615;      // title_screen: the logo drops in (title_logo_drop)
// collision_box_event, the service station's driveway box (C358h; the player only): stopping there
// repairs the car. Called every frame the car is in the box.
constexpr uint16_t kServiceStation = 0x1B74;
// The keyboard handler in segment 3FFC, past updating its key bitmap (game keyboard mode only).
constexpr uint16_t kKeyboard = emu_seg(0x3FFC);
constexpr uint16_t kKeyUpdated = 0x0219;
constexpr uint16_t kKeyHeld = 0x000F;     // 11 bytes: bit 80h >> (n & 7) of byte n >> 3, n = scan code - 1
constexpr uint16_t kKeyboardMode = 0x003C;  // b: 1 while the game reads the keyboard itself (races)
// snd_stop callers that aren't the game silencing everything: the engine noise switched off (E key
// and every frame after), and the crash into a car or pedestrian.
constexpr uint16_t kRetEngineOff = 0x0C28, kRetEngineOffFrame = 0x9577, kRetContactStop = 0x1720;

// Driver variables (cs:).
constexpr uint16_t kRequest = 0x926C;  // b: bit 1 skid, 2 siren, 3 engine (set by the race)
constexpr uint16_t kEngineOn = 0x926E;  // b: engine noise on (E key)
constexpr uint16_t kEnabled = 0x9277;   // b: sound on (S key)
constexpr uint16_t kEngineDivisor = 0x92A8;
constexpr uint16_t kSlide = 0x92BA;
constexpr uint16_t kMenuSaved = 0xE5FC;
constexpr uint16_t kEngineSpeed = 0x5891;  // revs, 100 rpm units
constexpr uint16_t kModel = 0x8DAE;        // player car model 0..3
constexpr uint16_t kRedlineTable = 0x586F, kIdleTable = 0x5887;  // w[model]
constexpr uint8_t kSkidBit = 2, kSirenBit = 4;

// Sequences (cs:).
constexpr uint16_t kSeqSkid = 0x927E, kSeqSiren = 0x9292, kSeqEngine = 0x92A6, kSeqWin = 0x92C4,
                   kSeqTitle = 0x9304;

// Who called: return addresses on the stack at the watch points.
constexpr uint16_t kRetEngineTarget = 0x9427;  // snd_engine_set_target's call to snd_play
constexpr uint16_t kRetGarageRev = 0x8BDC;     // garage_rev's call to snd_engine_set_target
constexpr uint16_t kRetPedestrianDown = 0x16D5, kRetContact = 0x1723, kRetObject = 0x1CF9, kRetRail = 0x7EC8,
                   kRetHighwayCar = 0x7F7C;
constexpr uint16_t kPlayer = 0x2D35;  // DS: player car; SI at the contact crash when it was a vehicle

// garage_rev's starting pitch: `mov word cs:[92A8], imm16` at 3009:8BCB.
constexpr uint16_t kGarageDivisorMov = 0x8BCB;
constexpr std::array<uint8_t, 5> kGarageDivisorOp{0x2E, 0xC7, 0x06, 0xA8, 0x92};

// The noise routines' parameters are immediates: mov bx / mov dx / mov bp, and the AND mask.
struct NoiseCode {
    uint16_t bx, dx, bp, mask;  // offsets of the imm16s
    bool off_from_on;
};
constexpr NoiseCode kCrashCode{0x94A2, 0x94A5, 0x94A8, 0x94B7, false};
constexpr NoiseCode kGrindCode{0x94D3, 0x94D6, 0x94D9, 0x94E8, true};

// Player state (DS:).
constexpr uint16_t kGear = 0x2D45, kMaxGear = 0x2D47, kThrottle = 0x2D48;
constexpr uint16_t kZ = 0x2D39, kPitch = 0x2D3D, kSpeed = 0x2D43;
constexpr uint16_t kHighway = 0x2AD4;     // b: 0 in the city
constexpr uint16_t kHelicopter = 0x2ACF;  // b: FFh in the helicopter view
constexpr uint16_t kStartLight = 0x2AD8;  // b: 0..4 during the start countdown, then 5
constexpr uint16_t kStopped = 0x2C59;     // w: 1 once the police have pulled you over

// The unused beeps (snd_beep_lo 95A4, snd_beep_hi 95C0), and the `mov ax, seq` that plays each.
constexpr uint16_t kSeqBeepLo = 0x92AE, kSeqBeepHi = 0x92B4;
constexpr uint16_t kBeepLoMov = 0x95AD, kBeepHiMov = 0x95C9;

constexpr int kThudPitch = 7, kThudStep = 32, kThudSpeed = 140;

// snd_play: `cmp byte cs:[9277],0`, present once the game has unpacked itself.
constexpr uint16_t kSignatureAt = 0x933E;
constexpr std::array<uint8_t, 6> kSignature{0x2E, 0x80, 0x3E, 0x77, 0x92, 0x00};

// No race frame for this long: the race isn't running. Only a safety net: the game silences everything
// when a race stops (snd_stop). A frame can take a crash noise's 0.12 s on top of its own 0.25 s.
constexpr uint64_t kStaleNs = 1'000'000'000;
constexpr size_t kMaxEvents = 4096;

constexpr std::array<const char*, static_cast<size_t>(Sfx::Count)> kNames{
    "engine", "garage_rev", "skid", "siren", "title_tune", "win_tune",
    "crash", "crash_car", "crash_rail", "hit_pedestrian", "gear_grind",
    "horn", "helicopter", "countdown_beep", "countdown_go", "splash", "thud", "pulled_over",
    "intro_cable_car", "intro_car", "intro_logo", "service_station",
};
constexpr Sfx kHeld[] = {Sfx::Horn, Sfx::Helicopter};

// Speaker programs that loop until the game stops them; playing one again continues it.
bool loops(Sfx s) { return s != Sfx::TitleTune; }

} // namespace

const char* sfx_name(Sfx s) {
    const auto i = static_cast<size_t>(s);
    return i < kNames.size() ? kNames[i] : nullptr;
}

std::optional<Sfx> sfx_from_name(std::string_view name) {
    for (size_t i = 0; i < kNames.size(); ++i) {
        if (name == kNames[i]) {
            return static_cast<Sfx>(i);
        }
    }
    return std::nullopt;
}

SfxKind sfx_kind(Sfx s) {
    switch (s) {
    case Sfx::Crash:
    case Sfx::CrashCar:
    case Sfx::CrashRail:
    case Sfx::HitPedestrian:
    case Sfx::GearGrind:
        return SfxKind::Noise;
    case Sfx::Horn:
    case Sfx::Helicopter:
        return SfxKind::Held;
    case Sfx::CountdownBeep:
    case Sfx::CountdownGo:
    case Sfx::Splash:
    case Sfx::Thud:
    case Sfx::PulledOver:
    case Sfx::IntroCableCar:
    case Sfx::IntroCar:
    case Sfx::IntroLogo:
    case Sfx::ServiceStation:
        return SfxKind::Cue;
    default:
        return SfxKind::Tone;
    }
}

bool is_thud(const CarMotion& before, const CarMotion& after) {
    if (before.highway || after.highway || after.speed < kThudSpeed) {
        return false;
    }
    // A compression: the slope under the car turns upward (pitch is nose-up positive). Turning
    // downward (over a crest) unloads the suspension instead, and DOS cars never leave the ground.
    return after.pitch - before.pitch >= kThudPitch || std::abs(after.z - before.z) >= kThudStep;
}

float divisor_hz(uint16_t divisor) { return static_cast<float>(kPitHz / (divisor ? divisor : 0x10000)); }

SoundEvents::Program decode_sequence(const std::function<uint16_t(uint16_t)>& read16, uint16_t start) {
    SoundEvents::Program p;
    std::vector<uint16_t> at_step;  // code offset of each step
    uint16_t at = start;
    for (int n = 0; n < 256; ++n) {
        const uint16_t ticks = read16(at), divisor = read16(off16(at, 2));
        if (ticks == 0xFFFF) {
            return p;
        }
        if (ticks == 0) {
            const uint16_t target = static_cast<uint16_t>(at - divisor);
            const auto it = std::find(at_step.begin(), at_step.end(), target);
            if (it != at_step.end() && !p.steps.empty()) {
                p.loop_to = static_cast<int>(it - at_step.begin());
                ++p.steps.back().ticks;  // the jump entry takes a tick of its own; the last note goes on
            }
            return p;
        }
        at_step.push_back(at);
        p.steps.push_back({ticks, divisor == 0xFFFF ? 0.0f : divisor_hz(divisor)});
        at = off16(at, 4);
    }
    return p;
}

std::vector<SoundEvents::Pulse> noise_pulses(const NoiseParams& p, double cpu_hz, uint32_t seed) {
    // 286 cycles (host/cpu_timing.h): LOOP costs 10 per taken iteration and 4 for the last. Around the
    // on-wait run the random-number call (PIT read) and the port write (about 76 cycles), around the
    // off-wait the port write and the counters (about 21).
    constexpr double kLoop = 10, kOnExtra = 76 - 6, kOffExtra = 21 - 6;
    const auto us = [cpu_hz](double cycles) { return static_cast<float>(cycles * 1e6 / cpu_hz); };
    std::vector<SoundEvents::Pulse> out;
    uint32_t r = seed;
    uint16_t off = p.off_start;
    for (int i = 0; i < p.clicks; ++i) {
        r = r * 1103515245u + 12345u;
        const auto on = static_cast<uint16_t>((r >> 16) & p.mask);
        off = static_cast<uint16_t>(p.off_from_on ? on + p.off_step : off + p.off_step);
        const double on_n = on ? on : 0x10000, off_n = off ? off : 0x10000;  // LOOP with CX=0: 65536
        out.push_back({us(kLoop * on_n + kOnExtra), us(kLoop * off_n + kOffExtra)});
    }
    return out;
}

uint16_t engine_slide(uint16_t engine_speed, uint16_t last_speed, uint16_t slide) {
    if (engine_speed > last_speed) {
        return 0x8005;  // revs up: the divisor falls (pitch rises) at 5 * (85 - revs) per tick
    }
    if (engine_speed == last_speed && engine_speed >= 20) {
        return 0;  // steady: hold
    }
    // Revs down, or steady below 2000 rpm: the pitch falls by 40h of divisor per tick. Above 5300 rpm
    // a drop keeps the previous slide (so the pitch can go on rising).
    return engine_speed <= 0x35 ? 0x0040 : slide;
}

uint16_t engine_pitch_tick(uint16_t divisor, uint16_t slide, uint16_t engine_speed) {
    if (slide & 0x8000) {
        if (divisor < 0x1400 || (engine_speed <= 0x35 && divisor < 0x1500)) {
            return divisor;  // top pitch: 233 Hz, or 222 Hz up to 5300 rpm
        }
        const auto step = static_cast<uint16_t>((slide & 0x7FFF) * static_cast<uint16_t>(0x55 - engine_speed));
        return static_cast<uint16_t>(divisor - step);
    }
    if (divisor > 0xB000) {
        return divisor;  // bottom pitch: 26.5 Hz
    }
    return static_cast<uint16_t>(divisor + slide);
}

SoundEvents::SoundEvents(host::Machine& machine) : machine_(machine) {
    Cpu& cpu = machine_.cpu();
    const auto watch = [&](uint16_t off, auto fn) { watches_.push_back(cpu.add_watch(Cpu::linear(kCode, off), fn)); };
    watch(kPlayCommit, [this](Cpu&) { on_play(); });
    watch(kStop, [this](Cpu&) { on_stop(); });
    watch(kSeqEnd, [this](Cpu&) { set_playing(std::nullopt); });
    watch(kSetDivisor, [this](Cpu& c) { latched_ = c.regs.r[host::CX]; });
    watch(kCrashNoise, [this](Cpu&) { on_noise(true, false); });
    watch(kCrashNoiseEnd, [this](Cpu&) { on_noise(false, false); });
    watch(kGrindNoise, [this](Cpu&) { on_noise(true, true); });
    watch(kGrindNoiseEnd, [this](Cpu&) { on_noise(false, true); });
    watch(kDispatch, [this](Cpu&) { on_frame(); });
    watch(kMenuMute, [this](Cpu&) { in_menu_ = true; });
    watch(kMenuRestore, [this](Cpu&) { in_menu_ = false; });
    watch(kCountdown, [this](Cpu&) { on_countdown(); });
    watch(kIntoTheBay, [this](Cpu&) { cue(Sfx::Splash); });
    watch(kPulledOver, [this](Cpu&) {
        if (rd16(machine_.memory(), kData, kStopped) == 0) {  // police_step repeats until the ticket shows
            cue(Sfx::PulledOver);
        }
    });
    watch(kTitleReveal, [this](Cpu&) { intro_bell_ = false; });
    watch(kTitleCableCar, [this](Cpu&) {
        if (!intro_bell_) {
            intro_bell_ = true;
            cue(Sfx::IntroCableCar);
        }
    });
    watch(kTitleCar, [this](Cpu&) { cue(Sfx::IntroCar); });
    watch(kTitleLogo, [this](Cpu&) { cue(Sfx::IntroLogo); });
    watch(kServiceStation, [this](Cpu&) {
        // On each drive onto the driveway, as the Mac rings it: again only after a frame off it.
        station_hit_ = true;
        if (!station_in_) {
            station_in_ = true;
            cue(Sfx::ServiceStation);
        }
    });
    watches_.push_back(cpu.add_watch(Cpu::linear(kKeyboard, kKeyUpdated), [this](Cpu&) { update_held(); }));
}

SoundEvents::~SoundEvents() {
    for (const auto id : watches_) {
        machine_.cpu().remove_watch(id);
    }
}

void SoundEvents::take(std::vector<SoundEvent>& out) {
    update_held();  // ends a state whose race frames stopped without the game silencing anything
    out.insert(out.end(), events_.begin(), events_.end());
    events_.clear();
}

void SoundEvents::push(Sfx sfx, bool start) {
    if (events_.size() >= kMaxEvents) {
        events_.erase(events_.begin());
    }
    events_.push_back({machine_.emulated_ns(), sfx, start});
}

void SoundEvents::set_playing(std::optional<Sfx> sfx) {
    if (sfx == playing_) {
        return;
    }
    if (playing_) {
        push(*playing_, false);
    }
    playing_ = sfx;
    if (playing_) {
        push(*playing_, true);
    }
}

void SoundEvents::on_play() {
    const Cpu& cpu = machine_.cpu();
    Memory& m = machine_.memory();
    std::optional<Sfx> sfx;
    switch (cpu.regs.r[host::AX]) {
    case kSeqSkid:
        sfx = Sfx::Skid;
        break;
    case kSeqSiren:
        sfx = Sfx::Siren;
        break;
    case kSeqTitle:
        sfx = Sfx::TitleTune;
        break;
    case kSeqWin:
        sfx = Sfx::WinTune;
        break;
    case kSeqEngine: {
        // Always through snd_engine_set_target, from the race frame or from the garage's rev loop.
        const uint16_t ss = cpu.regs.s[host::SS], sp = cpu.regs.r[host::SP];
        const bool garage = rd16(m, ss, sp) == kRetEngineTarget && rd16(m, ss, off16(sp, 2)) == kRetGarageRev;
        sfx = garage ? Sfx::GarageRev : Sfx::Engine;
        break;
    }
    default:  // the idle sequence, or one of the unused beeps
        break;
    }
    if (sfx && sfx == playing_ && !loops(*sfx)) {
        push(*sfx, false);  // a one-shot played again starts over
        push(*sfx, true);
        return;
    }
    set_playing(sfx);
}

void SoundEvents::on_stop() {
    set_playing(std::nullopt);
    // Pause, menus, the race-over pictures, sound off: the game silences everything. The race's own
    // stops (engine noise off, a crash into a car or pedestrian) leave the other sounds alone.
    const Cpu& cpu = machine_.cpu();
    const uint16_t ret = rd16(machine_.memory(), cpu.regs.s[host::SS], cpu.regs.r[host::SP]);
    if (ret != kRetEngineOff && ret != kRetEngineOffFrame && ret != kRetContactStop) {
        silenced_ = true;
        update_held();
    }
}

void SoundEvents::cue(Sfx sfx) {
    if (sound_on()) {
        push(sfx, true);
    }
}

void SoundEvents::on_countdown() {
    // Called every frame of the countdown: light 1 ("buckle up"), light 2 ("get ready"), light 3 ("go").
    const int light = rd8(machine_.memory(), kData, kStartLight);
    if (light == 0 || light < countdown_) {
        countdown_ = 0;  // a new race
    }
    if (light >= 1 && light <= 3 && light != countdown_) {
        countdown_ = light;
        cue(light == 3 ? Sfx::CountdownGo : Sfx::CountdownBeep);
    }
}

void SoundEvents::update_held() {
    for (size_t i = 0; i < std::size(kHeld); ++i) {
        const bool want = requested(kHeld[i]);
        if (want != held_[i]) {
            held_[i] = want;
            push(kHeld[i], want);
        }
    }
}

void SoundEvents::on_noise(bool start, bool grind) {
    if (!start) {
        if (noise_) {
            push(*noise_, false);
            noise_.reset();
        }
        return;
    }
    Sfx sfx = Sfx::GearGrind;
    if (!grind) {
        const Cpu& cpu = machine_.cpu();
        switch (rd16(machine_.memory(), cpu.regs.s[host::SS], cpu.regs.r[host::SP])) {
        case kRetPedestrianDown:
            sfx = Sfx::HitPedestrian;
            break;
        case kRetContact:  // a vehicle, or a pedestrian knocked down now (SI = the pedestrian)
            sfx = cpu.regs.r[host::SI] == kPlayer ? Sfx::CrashCar : Sfx::HitPedestrian;
            break;
        case kRetRail:
            sfx = Sfx::CrashRail;
            break;
        case kRetHighwayCar:
            sfx = Sfx::CrashCar;
            break;
        case kRetObject:
        default:
            sfx = Sfx::Crash;
            break;
        }
    }
    noise_ = sfx;
    push(sfx, true);
}

void SoundEvents::on_frame() {
    Memory& m = machine_.memory();
    // The frame's input is still in the car struct here: the player step that clears it comes later.
    throttle_ = rd8(m, kData, kThrottle) != 0;
    const uint16_t gear = rd16(m, kData, kGear);
    gear_ = gear > rd8(m, kData, kMaxGear) ? -1 : gear;

    const bool was_running = race_running();
    frame_ns_ = machine_.emulated_ns();
    frame_seen_ = true;
    silenced_ = false;
    // The last player step's result (this frame's comes after the dispatcher).
    const CarMotion now{static_cast<int16_t>(rd16(m, kData, kZ)), static_cast<int16_t>(rd16(m, kData, kPitch)),
                        static_cast<int16_t>(rd16(m, kData, kSpeed)), rd8(m, kData, kHighway) != 0};
    if (was_running && motion_ && is_thud(*motion_, now)) {
        cue(Sfx::Thud);
    }
    motion_ = now;
    if (!station_hit_) {
        station_in_ = false;  // the last frame's box tests didn't find the car on the driveway
    }
    station_hit_ = false;
    update_held();
}

bool SoundEvents::race_running() const {
    return frame_seen_ && !silenced_ && machine_.emulated_ns() - frame_ns_ < kStaleNs;
}

bool SoundEvents::sound_on() const { return game_loaded() && rd8(machine_.memory(), kCode, kEnabled) != 0; }

bool SoundEvents::game_loaded() const {
    for (size_t i = 0; i < kSignature.size(); ++i) {
        if (rd8(machine_.memory(), kCode, static_cast<uint16_t>(kSignatureAt + i)) != kSignature[i]) {
            return false;
        }
    }
    return true;
}

bool SoundEvents::enabled() const {
    if (!game_loaded()) {
        return false;
    }
    return rd8(machine_.memory(), kCode, in_menu_ ? kMenuSaved : kEnabled) != 0;
}

bool SoundEvents::requested(Sfx sfx) const {
    const SfxKind kind = sfx_kind(sfx);
    if (kind == SfxKind::Cue) {
        return false;
    }
    if (kind != SfxKind::Held && sfx != Sfx::Engine && sfx != Sfx::Skid && sfx != Sfx::Siren) {
        return playing_ == sfx || noise_ == sfx;
    }
    Memory& m = machine_.memory();
    if (!race_running() || !sound_on()) {
        return false;
    }
    const uint8_t request = rd8(m, kCode, kRequest);
    switch (sfx) {
    case Sfx::Skid:
        return (request & kSkidBit) != 0;
    case Sfx::Siren:
        return (request & kSirenBit) != 0;
    case Sfx::Helicopter:
        return rd8(m, kData, kHelicopter) != 0;
    case Sfx::Horn: {
        constexpr int n = kHornScancode - 1;
        return rd8(m, kKeyboard, kKeyboardMode) == 1 &&
               (rd8(m, kKeyboard, static_cast<uint16_t>(kKeyHeld + (n >> 3))) & (0x80 >> (n & 7))) != 0;
    }
    default:
        return rd8(m, kCode, kEngineOn) != 0;
    }
}

std::optional<Sfx> SoundEvents::playing() const { return playing_; }

EngineSound SoundEvents::engine() const {
    EngineSound e;
    if (!game_loaded()) {
        return e;
    }
    Memory& m = machine_.memory();
    e.on = playing_ == Sfx::Engine;
    e.running = requested(Sfx::Engine);
    e.speaker_hz = e.on ? divisor_hz(latched_) : 0.0f;
    e.pitch_hz = divisor_hz(rd16(m, kCode, kEngineDivisor));
    const uint16_t slide = rd16(m, kCode, kSlide);
    e.slide = (slide & 0x7FFF) == 0 ? 0 : (slide & 0x8000) ? 1 : -1;
    e.rpm = rd16(m, kCode, kEngineSpeed) * 100.0f;
    if (const uint16_t model = rd16(m, kCode, kModel); model < 4) {
        e.idle_rpm = rd16(m, kCode, static_cast<uint16_t>(kIdleTable + 2 * model)) * 100.0f;
        e.redline_rpm = rd16(m, kCode, static_cast<uint16_t>(kRedlineTable + 2 * model)) * 100.0f;
    }
    e.throttle = throttle_;
    e.gear = gear_;
    return e;
}

SoundEvents::Program SoundEvents::program(Sfx sfx) const {
    if (!game_loaded()) {
        return {};
    }
    Memory& m = machine_.memory();
    const auto read16 = [&m](uint16_t off) { return rd16(m, kCode, off); };
    const auto noise = [&](const NoiseCode& c) {
        NoiseParams p;
        p.clicks = rd16(m, kCode, c.dx) + 1;  // dec dx / jns: dx down to 0
        p.mask = read16(c.mask);
        p.off_start = read16(c.bx);
        p.off_step = read16(c.bp);
        p.off_from_on = c.off_from_on;
        Program prog;
        prog.pulses = noise_pulses(p, static_cast<double>(machine_.cycle_at_ns(1'000'000'000) - 1));
        return prog;
    };
    switch (sfx) {
    case Sfx::Engine:
        return decode_sequence(read16, kSeqEngine);  // its divisor is the slide's current value
    case Sfx::GarageRev: {
        Program p = decode_sequence(read16, kSeqEngine);
        for (size_t i = 0; i < kGarageDivisorOp.size(); ++i) {
            if (rd8(m, kCode, static_cast<uint16_t>(kGarageDivisorMov + i)) != kGarageDivisorOp[i]) {
                return {};
            }
        }
        const float hz = divisor_hz(read16(static_cast<uint16_t>(kGarageDivisorMov + kGarageDivisorOp.size())));
        for (Step& s : p.steps) {
            s.hz = hz;
        }
        return p;
    }
    case Sfx::Skid:
        return decode_sequence(read16, kSeqSkid);
    case Sfx::Siren:
        return decode_sequence(read16, kSeqSiren);
    case Sfx::TitleTune:
        return decode_sequence(read16, kSeqTitle);
    case Sfx::WinTune:
        return decode_sequence(read16, kSeqWin);
    case Sfx::Crash:
    case Sfx::CrashCar:
    case Sfx::CrashRail:
    case Sfx::HitPedestrian:
        return noise(kCrashCode);
    case Sfx::GearGrind:
        return noise(kGrindCode);
    case Sfx::CountdownBeep:
    case Sfx::CountdownGo: {
        // The beeps the DOS game has but never plays, 659.6 and 880.6 Hz: likely a cut countdown.
        const bool lo = sfx == Sfx::CountdownBeep;
        const uint16_t mov = lo ? kBeepLoMov : kBeepHiMov, seq = lo ? kSeqBeepLo : kSeqBeepHi;
        if (rd8(m, kCode, mov) != 0xB8 || read16(static_cast<uint16_t>(mov + 1)) != seq) {
            return {};
        }
        return decode_sequence(read16, seq);
    }
    default:  // no sound in the DOS game
        break;
    }
    return {};
}

} // namespace vette::game
