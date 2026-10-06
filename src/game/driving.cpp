#include "game/driving.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "game/x86.h"

namespace vette::game {
namespace {

constexpr uint16_t kCode = emu_seg(0x3009);
constexpr uint16_t kData = kDataSeg;
constexpr uint16_t kSkidSeg = emu_seg(0x4160);

// Layer points (re/notes/11-driving.md). Watches run before the original instruction, which then
// executes as usual unless the watch moved CS:IP; the steer/skid routine is replaced by a code hook that
// returns for it.
constexpr uint16_t kHeadingUpdate = 0x01A5;     // frame loop: `mov ax,[2D3B]`, then heading += wheel
constexpr uint16_t kHeadingStore = 0x01C3;      // frame loop: `mov [2D3B],ax`
constexpr uint16_t kAfterCamera = 0x0248;       // frame loop: `call 12D2`, the camera just placed (1D89)
constexpr uint16_t kBeforeDrivetrain = 0x0ED9;  // player_step: `call 1EC8` (engine, gearbox, drivetrain)
constexpr uint16_t kBeforeMove = 0x0EDC;        // player_step: `call 1738` (vehicle_move)
constexpr uint16_t kAfterStep = 0x17AD;         // vehicle_move: `mov dx,[3261]`, the step's dx/dy (3CE7) to add
constexpr uint16_t kGroundSet = 0x1858;         // vehicle_move: `jmp 1887` after the player's z and pitch
constexpr uint16_t kSteerSkid = 0x037F;         // player_steer_skid (4160), far

// The DOS 1.1 build's code at those points.
struct Signature {
    uint16_t seg, off;
    std::array<uint8_t, 5> bytes;
    size_t n;
};
constexpr Signature kSignatures[] = {
    {kCode, kHeadingUpdate, {0xA1, 0x3B, 0x2D}, 3},
    {kCode, kHeadingStore, {0xA3, 0x3B, 0x2D}, 3},
    {kCode, kAfterCamera, {0xE8, 0x87, 0x10}, 3},
    {kCode, kBeforeDrivetrain, {0xE8, 0xEC, 0x0F}, 3},
    {kCode, kBeforeMove, {0xE8, 0x59, 0x08}, 3},
    {kCode, kAfterStep, {0x8B, 0x16, 0x61, 0x32}, 4},
    {kCode, kGroundSet, {0xEB, 0x2D}, 2},
    {kSkidSeg, kSteerSkid, {0x83, 0x3E, 0x4B, 0x2C, 0x00}, 5},
};

// The player's car (DS:2D35, notes 04 section 2) and the state around it.
constexpr uint16_t kX = 0x2D35, kY = 0x2D37, kZ = 0x2D39, kHeading = 0x2D3B, kPitch = 0x2D3D, kRoll = 0x2D3F;
constexpr uint16_t kSpeed = 0x2D43, kStep = 0x2D55, kRow = 0x2D57, kCol = 0x2D59;
constexpr uint16_t kStepDx = 0x3261, kStepDy = 0x3263;  // the step along the travel direction (3CE7's)
constexpr uint16_t kVehicle = 0x2C51;         // w: 0 while vehicle_move moves the player
constexpr uint16_t kTravel = 0x2C45, kTravelPitch = 0x2C47, kTravelRoll = 0x2C49, kSkid = 0x2C4B;
constexpr uint16_t kSteer = 0x2B84;           // w: the wheel, degrees per frame (+ = right)
constexpr uint16_t kModelCopy = 0x2C67;       // w: the car model (1E4C copies cs:8DAE here)
constexpr uint16_t kLevel = 0xFC4F;           // b: difficulty 0..2
constexpr uint16_t kGrip = 0x2BF1;            // w[model * 9 + |steer|]: skid speed
constexpr uint16_t kGripBonus = 0x0373;       // 4160: b[model + 4 * level], added to it
constexpr uint16_t kRough = 0x2C6D;           // b: on a rough surface
constexpr uint16_t kReverse = 0x2AD5;         // b: reversing
constexpr uint16_t kFrameRate = 0x2CD3;       // w: frames per second, >= 4
constexpr uint16_t kHighway = 0x2AD4;         // b: freeway mode when non-zero
constexpr uint16_t kStartLight = 0x2AD8;      // b: 5 once the race is on
constexpr uint16_t kCamPitch = 0x2C79;        // w: the view's pitch, degrees (from 2BEB)
constexpr uint16_t kChaseDistance = 0x2C7D;   // w: 0 in the car
constexpr uint16_t kHelicopter = 0x2ACF;      // b: FFh in the helicopter view

constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kGapNs = 400'000'000;      // no player frame for this long: start the state afresh
constexpr double kTeleport = 600;             // units moved in one frame: the game placed the car

double wrap180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

int wrap360(int a) {
    a %= 360;
    return a < 0 ? a + 360 : a;
}

int16_t s16(uint16_t v) { return static_cast<int16_t>(v); }
uint16_t u16(int v) { return static_cast<uint16_t>(v); }

// Absolute position: the big tile (DS:2D57 row, 2D59 column) times 8000h plus the local coordinate.
double absolute(Memory& m, uint16_t local, uint16_t tile) {
    return static_cast<double>(s16(rd16(m, kData, tile))) * 0x8000 + rd16(m, kData, local);
}

} // namespace

bool set_tuning(DrivingTuning& t, std::string_view name, float value) {
    static constexpr struct {
        std::string_view name;
        float DrivingTuning::*member;
    } kMembers[] = {
        {"drift_onset", &DrivingTuning::drift_onset},
        {"drift_per_grip", &DrivingTuning::drift_per_grip},
        {"drift_max", &DrivingTuning::drift_max},
        {"drift_build", &DrivingTuning::drift_build},
        {"drift_relax", &DrivingTuning::drift_relax},
        {"drift_sound", &DrivingTuning::drift_sound},
        {"drift_scrub", &DrivingTuning::drift_scrub},
        {"gravity", &DrivingTuning::gravity},
        {"lift_off", &DrivingTuning::lift_off},
        {"jump_min_speed", &DrivingTuning::jump_min_speed},
        {"hard_landing", &DrivingTuning::hard_landing},
        {"air_pitch_rate", &DrivingTuning::air_pitch_rate},
        {"air_pitch_max", &DrivingTuning::air_pitch_max},
        {"air_view_max", &DrivingTuning::air_view_max},
        {"lane_max_rate", &DrivingTuning::lane_max_rate},
        {"lane_heading_gain", &DrivingTuning::lane_heading_gain},
        {"lane_offset_gain", &DrivingTuning::lane_offset_gain},
        {"lane_max_correction", &DrivingTuning::lane_max_correction},
        {"lane_tolerance", &DrivingTuning::lane_tolerance},
        {"lane_max_angle", &DrivingTuning::lane_max_angle},
        {"lane_min_speed", &DrivingTuning::lane_min_speed},
    };
    for (const auto& m : kMembers) {
        if (m.name == name) {
            t.*m.member = value;
            return true;
        }
    }
    return false;
}

// --- Lane markings ------------------------------------------------------------------------------------

LaneMap::LaneMap(std::vector<LaneLine> lines)
    : lines_(std::move(lines)), cells_(static_cast<size_t>(kCells) * kCells) {
    for (size_t i = 0; i < lines_.size(); ++i) {
        const LaneLine& l = lines_[i];
        const int cx0 = std::clamp(std::min(l.x0, l.x1) / kCellSize, 0, kCells - 1);
        const int cx1 = std::clamp(std::max(l.x0, l.x1) / kCellSize, 0, kCells - 1);
        const int cy0 = std::clamp(std::min(l.y0, l.y1) / kCellSize, 0, kCells - 1);
        const int cy1 = std::clamp(std::max(l.y0, l.y1) / kCellSize, 0, kCells - 1);
        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                cells_[static_cast<size_t>(cx * kCells + cy)].push_back(static_cast<uint32_t>(i));
            }
        }
    }
}

std::optional<LaneMap::Fix> LaneMap::find(double x, double y, double heading_deg, double max_angle) const {
    constexpr double kMinLength = 96;   // dashes are 128 long; shorter lines are other markings
    constexpr double kAlong = 320;      // look this far ahead and behind (a dash and its gap are 256)
    constexpr double kNear = 72;        // one marking beside the car: within this
    constexpr double kMinWidth = 40, kMaxWidth = 100;
    const double h = heading_deg * kPi / 180;
    const double dx = std::cos(h), dy = std::sin(h);  // x north, y east: heading 0 = north, 90 = east
    const int cx = static_cast<int>(std::floor(x / kCellSize)), cy = static_cast<int>(std::floor(y / kCellSize));
    double right = std::numeric_limits<double>::infinity(), left = -right;
    double right_dir = 0, left_dir = 0;
    for (int ix = cx - 1; ix <= cx + 1; ++ix) {
        for (int iy = cy - 1; iy <= cy + 1; ++iy) {
            if (ix < 0 || iy < 0 || ix >= kCells || iy >= kCells) continue;
            for (const uint32_t i : cells_[static_cast<size_t>(ix * kCells + iy)]) {
                const LaneLine& l = lines_[i];
                double ux = l.x1 - l.x0, uy = l.y1 - l.y0;
                const double len = std::hypot(ux, uy);
                if (len < kMinLength) continue;
                ux /= len;
                uy /= len;
                if (ux * dx + uy * dy < 0) {  // run it the car's way
                    ux = -ux;
                    uy = -uy;
                }
                const double angle = std::atan2(uy, ux) * 180 / kPi;
                if (std::fabs(wrap180(angle - heading_deg)) > max_angle) continue;
                const double t0 = (l.x0 - x) * dx + (l.y0 - y) * dy, t1 = (l.x1 - x) * dx + (l.y1 - y) * dy;
                if (std::max(t0, t1) < -kAlong || std::min(t0, t1) > kAlong) continue;
                // Signed distance from the car to the line, + to the right (the line's right normal).
                const double s = (l.x0 - x) * -uy + (l.y0 - y) * ux;
                if (s >= 0 && s < right) {
                    right = s;
                    right_dir = angle;
                } else if (s < 0 && s > left) {
                    left = s;
                    left_dir = angle;
                }
            }
        }
    }
    const bool has_right = right <= kMaxWidth, has_left = -left <= kMaxWidth;
    Fix fix;
    if (has_right && has_left && right - left >= kMinWidth && right - left <= kMaxWidth) {
        fix.offset = (left + right) / 2;
        fix.direction = left_dir + wrap180(right_dir - left_dir) / 2;
    } else if (has_right && (!has_left || right <= -left) && right <= kNear) {
        fix.offset = right - kLaneHalf;
        fix.direction = right_dir;
    } else if (has_left && -left <= kNear) {
        fix.offset = left + kLaneHalf;
        fix.direction = left_dir;
    } else {
        return std::nullopt;
    }
    fix.direction = std::fmod(fix.direction + 360.0, 360.0);
    return fix;
}

// --- The layers ---------------------------------------------------------------------------------------

Driving::Driving(host::Machine& machine, Options options, DrivingTuning tuning)
    : machine_(machine), options_(options), tuning_(tuning) {
    if (!options_.improved && !options_.lane_centering) {
        return;  // the game untouched
    }
    Cpu& cpu = machine_.cpu();
    const auto watch = [&](uint16_t off, host::Cpu::Watch fn) {
        watches_.push_back(cpu.add_watch(Cpu::linear(kCode, off), std::move(fn)));
    };
    watch(kHeadingUpdate, [this](Cpu& c) { heading_update(c); });
    watch(kAfterStep, [this](Cpu&) { exact_step(); });
    watch(kGroundSet, [this](Cpu&) { after_ground(); });
    if (options_.improved) {
        watch(kAfterCamera, [this](Cpu&) { view_pitch(); });
        watch(kBeforeDrivetrain, [this](Cpu&) { before_drivetrain(); });
        watch(kBeforeMove, [this](Cpu&) { before_move(); });
        // The steer/skid replacement goes in once ready() has seen the expected code.
    }
}

Driving::~Driving() { remove(); }

void Driving::remove() {
    Cpu& cpu = machine_.cpu();
    for (const host::Cpu::WatchId id : watches_) {
        cpu.remove_watch(id);
    }
    watches_.clear();
    if (code_hook_) {
        cpu.clear_code_hook(*code_hook_);
        code_hook_.reset();
    }
}

bool Driving::ready() {
    if (check_ == Check::Pending) {
        // A race frame: VETTE.EXE has unpacked itself. Is it the build these addresses are for?
        Memory& m = machine_.memory();
        check_ = Check::Ok;
        for (const Signature& s : kSignatures) {
            for (size_t i = 0; i < s.n; ++i) {
                if (rd8(m, s.seg, static_cast<uint16_t>(s.off + i)) != s.bytes[i]) {
                    check_ = Check::Bad;
                }
            }
        }
        if (check_ == Check::Bad) {
            remove();  // (safe from inside a watch: the CPU runs a copy of the list)
        } else if (options_.improved) {
            code_hook_ = Cpu::linear(kSkidSeg, kSteerSkid);
            machine_.cpu().set_code_hook(*code_hook_, [this](Cpu& c) { steer_skid(c); });
        }
    }
    return check_ == Check::Ok;
}

void Driving::exact_step() {
    Memory& m = machine_.memory();
    if (!ready() || rd16(m, kData, kVehicle) != 0 || rd8(m, kData, kHighway)) {
        return;  // another vehicle, or the freeway (its own coordinates and lanes)
    }
    // The original rounds each step's dx and dy down (3CE7), so a car a degree or two off an axis
    // creeps sideways one way at a unit a frame and not at all the other way. Carry the sideways part
    // of what the rounding lost over to later frames instead: the car goes where it points. (The part
    // along the travel direction stays the original's: the speed is unchanged.)
    const double d = s16(rd16(m, kData, kStep));
    const uint64_t now = machine_.emulated_ns();
    if (now - step_ns_ > kGapNs) {
        lateral_ = 0;
    }
    step_ns_ = now;
    if (d == 0) {
        return;
    }
    const double h = s16(rd16(m, kData, kTravel)) * kPi / 180;
    const double along = d * std::cos(s16(rd16(m, kData, kTravelPitch)) * kPi / 180);
    const double nx = -std::sin(h), ny = std::cos(h);  // the travel direction's right
    const int dx = s16(rd16(m, kData, kStepDx)), dy = s16(rd16(m, kData, kStepDy));
    lateral_ += (along * std::cos(h) - dx) * nx + (along * std::sin(h) - dy) * ny;
    lateral_ = std::clamp(lateral_, -3.0, 3.0);
    if (std::fabs(lateral_) < 1) {
        return;
    }
    // A unit along the axis nearest the sideways direction.
    const bool on_x = std::fabs(nx) >= std::fabs(ny);
    const double n = on_x ? nx : ny;
    const int unit = (lateral_ > 0) == (n > 0) ? 1 : -1;
    if (on_x) {
        wr16(m, kData, kStepDx, u16(dx + unit));
    } else {
        wr16(m, kData, kStepDy, u16(dy + unit));
    }
    lateral_ -= unit * n;
}

double Driving::frame_dt() const {
    const int fr = std::max<int>(4, s16(rd16(machine_.memory(), kData, kFrameRate)));
    return 1.0 / fr;
}

void Driving::heading_update(Cpu& c) {
    if (!ready()) {
        return;
    }
    // A new frame (the sound dispatcher, which reads flying(), has run).
    landed_ = false;
    if (airborne_ && (rd8(machine_.memory(), kData, kHighway) || machine_.emulated_ns() - last_ns_ > kGapNs)) {
        // On the freeway (the game sets its own heights there) or after a pause: back on the ground.
        airborne_ = false;
        valid_ = false;
    }
    if (airborne_) {
        // No grip in the air: the wheel doesn't turn the car. Skip `heading += wheel` (01A8-01C2).
        c.regs.r[host::AX] = rd16(machine_.memory(), kData, kHeading);
        c.regs.ip = kHeadingStore;
        return;
    }
    if (options_.lane_centering) {
        lane_assist();
    }
}

void Driving::view_pitch() {
    if (!ready() || !airborne_) {
        return;
    }
    Memory& m = machine_.memory();
    if (rd16(m, kData, kChaseDistance) != 0 || rd8(m, kData, kHelicopter) != 0) {
        return;  // the cockpit view only
    }
    // The original's view never pitches with the car (ramps included); in flight it follows the nose
    // from where it left the ground.
    const double tilt = std::clamp<double>(pitch_ - launch_pitch_, -tuning_.air_view_max, tuning_.air_view_max);
    wr16(m, kData, kCamPitch, u16(s16(rd16(m, kData, kCamPitch)) + static_cast<int>(std::lround(tilt))));
}

void Driving::before_drivetrain() {
    if (!ready()) {
        return;
    }
    speed_before_ = s16(rd16(machine_.memory(), kData, kSpeed));
}

void Driving::before_move() {
    if (!ready() || !airborne_) {
        return;
    }
    // No traction in the air: the car keeps the speed it had (the engine still revs freely), and covers
    // all of it (the original takes twice the wheel slip 2C4D off the step).
    Memory& m = machine_.memory();
    const int speed = std::max(0, speed_before_);
    wr16(m, kData, kSpeed, u16(speed));
    const int fr = std::max<int>(4, s16(rd16(m, kData, kFrameRate)));
    wr16(m, kData, kStep, u16((rd8(m, kData, kReverse) ? -speed : speed) / fr));
}

void Driving::steer_skid(Cpu& c) {
    Memory& m = machine_.memory();
    const double dt = frame_dt();
    const int facing = rd16(m, kData, kHeading);
    const int steer = s16(rd16(m, kData, kSteer));
    int speed = s16(rd16(m, kData, kSpeed));

    if (airborne_) {
        // The travel direction stays as it was at take-off; the skid sound stops; the flight is
        // ballistic (vertical motion is after_ground's), so all of the step is horizontal.
        wr16(m, kData, kTravel, u16(travel_));
        wr16(m, kData, kSkid, 0);
        wr16(m, kData, kTravelPitch, 0);
    } else {
        // How far past the original's skid speed for this much lock the car is (its grip table and the
        // difficulty's bonus, as player_steer_skid reads them).
        const int lock = std::min(8, std::abs(steer));
        double beyond = 0;
        if (lock >= 2 && !rd8(m, kData, kReverse)) {
            const int model = std::clamp<int>(rd16(m, kData, kModelCopy), 0, 3);
            const int level = std::clamp<int>(rd8(m, kData, kLevel), 0, 2);
            const int grip = rd16(m, kData, u16(kGrip + 2 * (model * 9 + lock))) +
                             rd8(m, kSkidSeg, u16(kGripBonus + model + 4 * level));
            beyond = grip > 0 ? static_cast<double>(speed) / grip - tuning_.drift_onset : 0;
        }
        double target = 0;
        if (beyond > 0) {
            target = std::min<double>(tuning_.drift_max, tuning_.drift_per_grip * beyond) * (steer > 0 ? 1 : -1);
        }
        // Toward the target: quicker as the slide builds than as it settles.
        const bool growing = std::fabs(target) > std::fabs(slip_) && target * slip_ >= 0;
        const double rate = (growing ? tuning_.drift_build : tuning_.drift_relax) * dt;
        slip_ += std::clamp(target - slip_, -rate, rate);
        travel_ = wrap360(facing - static_cast<int>(std::lround(slip_)));
        wr16(m, kData, kTravel, u16(travel_));
        wr16(m, kData, kSkid, std::fabs(slip_) >= tuning_.drift_sound ? 1 : 0);
        // A slide scrubs speed.
        scrub_ += tuning_.drift_scrub * std::fabs(slip_) * dt * (rd8(m, kData, kRough) ? 2 : 1);
        const int loss = static_cast<int>(scrub_);
        scrub_ -= loss;
        if (loss > 0) {
            speed = std::max(0, speed - loss);
            wr16(m, kData, kSpeed, u16(speed));
        }
        wr16(m, kData, kTravelPitch, rd16(m, kData, kPitch));
    }
    wr16(m, kData, kTravelRoll, rd16(m, kData, kRoll));
    tm_.slip = slip_;
    // RETF.
    c.regs.ip = c.pop16();
    c.regs.s[host::CS] = c.pop16();
}

void Driving::after_ground() {
    if (!ready()) {
        return;
    }
    Memory& m = machine_.memory();
    const double dt = frame_dt();
    const uint64_t now = machine_.emulated_ns();
    const int ground = s16(rd16(m, kData, kZ));
    const int ground_pitch = s16(rd16(m, kData, kPitch));
    const int speed = s16(rd16(m, kData, kSpeed));
    const double x = absolute(m, kX, kRow), y = absolute(m, kY, kCol);

    landed_ = false;
    const bool gap = !valid_ || now - last_ns_ > kGapNs || std::fabs(x - x_prev_) + std::fabs(y - y_prev_) > kTeleport;
    if (gap || speed == 0) {
        slip_ = 0;  // a stopped car (a crash) sets off straight
    }
    if (gap || !options_.improved) {
        valid_ = true;
        airborne_ = false;
        z_ = ground;
        vz_ = 0;
        pitch_ = ground_pitch;
    } else {
        const double ground_vz = (ground - ground_prev_) / dt;
        if (!airborne_) {
            // On the ground the car rises and falls with it. When the ground drops away faster than the
            // suspension can follow (over a crest at speed), the car keeps its vertical speed and flies.
            if (speed >= tuning_.jump_min_speed && vz_ - ground_vz > tuning_.lift_off) {
                airborne_ = true;
                vz_ -= tuning_.lift_off;
                air_time_ = 0;
                launch_pitch_ = pitch_;
                ++tm_.jumps;
            } else {
                z_ = ground;
                vz_ = ground_vz;
                pitch_ = ground_pitch;
            }
        }
        if (airborne_) {
            vz_ -= tuning_.gravity * dt;
            z_ += vz_ * dt;
            air_time_ += dt;
            // The nose follows the flight path.
            const double path = std::atan2(vz_, std::max(1, speed)) * 180 / kPi;
            const double target = std::clamp<double>(path, -tuning_.air_pitch_max, tuning_.air_pitch_max);
            const double rate = tuning_.air_pitch_rate * dt;
            pitch_ += std::clamp(target - pitch_, -rate, rate);
            if (z_ <= ground) {
                const double impact = ground_vz - vz_;
                airborne_ = false;
                landed_ = true;
                z_ = ground;
                vz_ = ground_vz;
                pitch_ = ground_pitch;
                ++tm_.landings;
                tm_.last_impact = impact;
                if (impact >= tuning_.hard_landing) {
                    ++tm_.hard_landings;
                    if (on_hard_landing) on_hard_landing(static_cast<float>(impact));
                }
            }
        }
        if (airborne_) {
            wr16(m, kData, kZ, u16(static_cast<int>(std::lround(z_))));
            wr16(m, kData, kPitch, u16(static_cast<int>(std::lround(pitch_))));
        }
    }
    ground_prev_ = ground;
    x_prev_ = x;
    y_prev_ = y;
    last_ns_ = now;

    tm_.t_ns = now;
    tm_.frame_rate = static_cast<int>(std::lround(1 / dt));
    tm_.x = x;
    tm_.y = y;
    tm_.z = s16(rd16(m, kData, kZ));
    tm_.ground_z = ground;
    tm_.vz = vz_;
    tm_.airborne = airborne_;
    tm_.air_time = air_time_;
    tm_.heading = rd16(m, kData, kHeading);
    tm_.travel = rd16(m, kData, kTravel);
    tm_.pitch = s16(rd16(m, kData, kPitch));
    tm_.speed = speed;
    tm_.steer = s16(rd16(m, kData, kSteer));
    tm_.skid = rd16(m, kData, kSkid) != 0;
    if (on_frame) on_frame(tm_);
}

void Driving::lane_assist() {
    Memory& m = machine_.memory();
    tm_.lane = false;
    tm_.assist_rate = 0;
    const int speed = s16(rd16(m, kData, kSpeed));
    const int steer = s16(rd16(m, kData, kSteer));
    if (!lanes_ || steer != 0 || std::fabs(slip_) >= 2 || speed < tuning_.lane_min_speed ||
        rd8(m, kData, kHighway) || rd8(m, kData, kStartLight) < 5 || rd8(m, kData, kReverse)) {
        assist_ = 0;  // the player steers (or there's nothing to follow): hands off at once
        return;
    }
    const int heading = rd16(m, kData, kHeading);
    const auto fix = lanes_->find(absolute(m, kX, kRow), absolute(m, kY, kCol), heading, tuning_.lane_max_angle);
    if (!fix) {
        assist_ = 0;
        return;
    }
    // Toward the lane's direction, angled a little toward its centre. (The heading is whole degrees and
    // the original's move rounds down, so a car angled 1 degree off may not move sideways at all: the
    // angle toward the centre has to reach 2 degrees to get it there.)
    const double error = wrap180(fix->direction - heading);
    const double toward = std::fabs(fix->offset) <= tuning_.lane_tolerance
                              ? 0
                              : std::clamp<double>(tuning_.lane_offset_gain * fix->offset, -tuning_.lane_max_correction,
                                                   tuning_.lane_max_correction);
    const double rate = std::clamp<double>(tuning_.lane_heading_gain * (error + toward), -tuning_.lane_max_rate,
                                           tuning_.lane_max_rate);
    assist_ += rate * frame_dt();
    const int turn = static_cast<int>(assist_);  // whole degrees; the rest carries over
    assist_ -= turn;
    if (turn != 0) {
        wr16(m, kData, kHeading, u16(wrap360(heading + turn)));
    }
    tm_.lane = true;
    tm_.lane_offset = fix->offset;
    tm_.lane_error = error;
    tm_.assist_rate = rate;
}

} // namespace vette::game
