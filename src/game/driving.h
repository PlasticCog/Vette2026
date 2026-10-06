#pragma once
// Improved Driving and Lane Centering (Enhanced options): native code layered on the original's player
// physics in the hosted game. Only the player's car changes; the opponent, the police and the traffic
// keep the original's code. With both options off nothing is installed and the game is untouched
// (re/notes/11-driving.md).
//
// Improved Driving:
//  - Drift. The original's skid is on or off: past a speed set by its grip table (player_steer_skid
//    4160:037F) the travel direction (DS:2C45) lags the car's facing by a few degrees a frame and the
//    car loses 5 units/s a frame. Here a slip angle (facing minus travel direction) builds smoothly from a
//    little below that speed, grows with how far past it the car is, and relaxes when the steering
//    eases; a big slide scrubs some speed, and only a big one sounds the skid.
//  - Jumps. The original sets the car's height from the ground every frame (vehicle_move 3009:1838).
//    Here the car has a vertical velocity: on the ground it follows the ground, and when the ground
//    falls away faster than the suspension can follow (a crest, the top of a ramp at speed) the car
//    flies under gravity, without steering or traction, its nose (and the cockpit view) following its
//    flight path, until it meets the ground again. A hard landing is reported (on_hard_landing) for the
//    landing thud.
// Lane Centering: while the wheel is centred, a slight heading correction (a few degrees a second at
// most) toward the direction and centre of the lane the car is in, found from the road's lane markings
// (LaneMap, built from the extracted city: enhanced/lanes.h). Any steering suspends it at once; it stays
// off on the freeway.
// Both: the original rounds each frame's step down on both axes (3009:3CE7), so a car a degree off an
// axis creeps sideways one way and not at all the other. The sideways part of what the rounding loses
// is carried over instead, so the car goes where it points (the speed along the road is unchanged).
//
// Per-second quantities are scaled by the game's own frame rate (DS:2CD3), so the feel doesn't change
// with the emulated PC's speed. The tuning constants are DrivingTuning's members.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "host/machine.h"

namespace vette::game {

struct DrivingTuning {
    // Drift. The slide starts at drift_onset times the original's skid speed for the steering lock in use
    // (its grip table, with the difficulty's bonus); its target angle grows by drift_per_grip degrees per
    // 1.0 of speed beyond that (as a fraction of the skid speed), up to drift_max.
    // More drift: lower drift_onset, raise drift_per_grip / drift_max. Less: the opposite.
    float drift_onset = 0.85f;
    float drift_per_grip = 24.0f;  // degrees
    float drift_max = 18.0f;       // degrees
    float drift_build = 30.0f;     // degrees per second toward a bigger target
    float drift_relax = 20.0f;     // degrees per second back toward a smaller one
    float drift_sound = 8.0f;      // degrees: the skid sound from here on
    float drift_scrub = 5.0f;      // speed lost per second, per degree of slip (units/s^2); double on rough ground

    // Jumps (world units: 1 unit is about 3 inches; speed 16 units/s is 3 mph).
    float gravity = 128.7f;        // units/s^2: 9.81 m/s^2
    float lift_off = 25.0f;        // units/s: how much faster than the car the ground may fall away before
                                   // the car leaves it (the suspension). Higher: fewer, smaller jumps
    float jump_min_speed = 450.0f; // units/s (84 mph): below this the car always keeps to the ground
    float hard_landing = 45.0f;    // units/s downward relative to the ground: a hard landing (thud)
    float air_pitch_rate = 20.0f;  // degrees per second the nose follows the flight path
    float air_pitch_max = 12.0f;   // degrees
    float air_view_max = 10.0f;    // degrees the cockpit view tilts with the nose in flight (0: level)

    // Lane centering. More assist: raise lane_max_rate (and the gains); less: lower them.
    // The assist steers toward the lane's direction plus a small angle toward its centre, at most
    // lane_max_rate degrees a second.
    float lane_max_rate = 3.0f;        // degrees per second of heading correction, at most
    float lane_heading_gain = 1.0f;    // per second: how quickly a heading error is corrected
    float lane_offset_gain = 0.25f;    // degrees toward the centre per unit off it (lanes are 64 units wide)
    float lane_max_correction = 4.0f;  // degrees: the most it angles toward the centre
    float lane_tolerance = 6.0f;       // units: this close to the centre it only keeps the lane's direction
    float lane_max_angle = 15.0f;      // degrees: only this close to the lane's direction
    float lane_min_speed = 60.0f;    // units/s (11 mph)
};

// Sets the DrivingTuning member called `name` (as spelled above); false if there is none.
bool set_tuning(DrivingTuning& t, std::string_view name, float value);

// A lane marking on the ground: a white or yellow line, absolute world coordinates (x north, y east).
struct LaneLine {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The road's lane markings, indexed by map cell, and the lane a car is in.
class LaneMap {
public:
    explicit LaneMap(std::vector<LaneLine> lines);

    struct Fix {
        double offset = 0;     // the lane centre's position across the car's direction (units, + = right)
        double direction = 0;  // the lane's heading, degrees 0..360, the one nearest the car's
    };
    // The lane a car at absolute (x, y) heading `heading_deg` is in: between the nearest markings on
    // either side running within `max_angle` degrees of its direction, or half a lane (32 units) from the
    // one marking beside it. nullopt off the marked roads.
    std::optional<Fix> find(double x, double y, double heading_deg, double max_angle = 15) const;
    size_t size() const { return lines_.size(); }

    static constexpr int kCells = 80;
    static constexpr int kCellSize = 0x800;
    static constexpr double kLaneHalf = 32;  // lanes are 64 units wide

private:
    std::vector<LaneLine> lines_;
    std::vector<std::vector<uint32_t>> cells_;  // line indices per cell (cx * kCells + cy)
};

class Driving {
public:
    struct Options {
        bool improved = false;
        bool lane_centering = false;
    };
    // Installs the hooks the options need (none if both are off). They check, the first time the game
    // reaches one, that the code there is the expected build's, and uninstall themselves if not.
    Driving(host::Machine& machine, Options options, DrivingTuning tuning = {});
    ~Driving();
    Driving(const Driving&) = delete;
    Driving& operator=(const Driving&) = delete;

    const Options& options() const { return options_; }
    const DrivingTuning& tuning() const { return tuning_; }
    void set_tuning(const DrivingTuning& t) { tuning_ = t; }

    // Lane centering needs the lane markings; it stays idle until they are given.
    void set_lanes(std::shared_ptr<const LaneMap> lanes) { lanes_ = std::move(lanes); }
    bool has_lanes() const { return lanes_ != nullptr; }

    // Called from inside the emulation at a hard landing, with the downward speed (units/s).
    std::function<void(float impact)> on_hard_landing;
    // True while the car is in the air, and on the race frame it lands: the original's own height
    // changes don't apply (the thud observer skips them; the landing reports itself).
    bool flying() const { return airborne_ || landed_; }

    // The player's state as the layers saw it on the last race frame (for logs and tests).
    struct Telemetry {
        uint64_t t_ns = 0;
        int frame_rate = 0;
        double x = 0, y = 0;   // absolute
        double z = 0, ground_z = 0, vz = 0;
        bool airborne = false;
        double air_time = 0;   // seconds in the current (or last) flight
        int heading = 0, travel = 0, pitch = 0, speed = 0, steer = 0;
        double slip = 0;       // degrees, facing minus travel direction
        bool skid = false;     // the skid flag (sound)
        bool lane = false;     // a lane was found (lane centering)
        double lane_offset = 0, lane_error = 0, assist_rate = 0;  // units, degrees, degrees/s
        int landings = 0, hard_landings = 0, jumps = 0;
        double last_impact = 0;  // units/s, the last landing's
    };
    const Telemetry& telemetry() const { return tm_; }
    // Called after each race frame's player step with the telemetry (optional).
    std::function<void(const Telemetry&)> on_frame;

private:
    bool ready();                   // the code is the expected build's (checked once)
    void remove();
    void heading_update(host::Cpu& c);  // frame loop, before the heading update (3009:01A5)
    void view_pitch();              // frame loop, after the camera is placed (3009:0248)
    void before_drivetrain();       // player_step, before player_drivetrain (3009:0ED9)
    void before_move();             // player_step, before vehicle_move (3009:0EDC)
    void exact_step();              // vehicle_move, the step computed, before it's added (3009:17AD)
    void after_ground();            // vehicle_move, the player's height and pitch just set (3009:1858)
    void steer_skid(host::Cpu& c);  // replaces player_steer_skid (4160:037F)
    void lane_assist();
    double frame_dt() const;

    host::Machine& machine_;
    Options options_;
    DrivingTuning tuning_;
    std::shared_ptr<const LaneMap> lanes_;
    std::vector<host::Cpu::WatchId> watches_;
    std::optional<uint32_t> code_hook_;  // linear address of the steer/skid replacement
    enum class Check { Pending, Ok, Bad } check_ = Check::Pending;

    // Drift.
    double slip_ = 0;
    double scrub_ = 0;  // fractional speed loss carried over
    int travel_ = 0;    // the travel direction set on the last ground frame

    // Vertical motion.
    bool valid_ = false;     // the state below follows the car (reset after gaps and jumps in position)
    bool airborne_ = false;
    bool landed_ = false;    // landed on the last player step
    double z_ = 0, vz_ = 0, pitch_ = 0, launch_pitch_ = 0, air_time_ = 0;
    int ground_prev_ = 0;
    double x_prev_ = 0, y_prev_ = 0;
    uint64_t last_ns_ = 0;
    int speed_before_ = 0;   // before this frame's drivetrain step

    // Sideways movement the original's rounding lost, carried over (units).
    double lateral_ = 0;
    uint64_t step_ns_ = 0;

    // Lane centering.
    double assist_ = 0;  // fractional degrees carried over

    Telemetry tm_;
};

} // namespace vette::game
