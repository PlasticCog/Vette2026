#pragma once
// Native ports of the original fixed-point 3D math (re/notes/03-renderer-and-visibility.md, "Camera,
// transform and fixed-point formats").
//
// Each routine has two layers:
//  - a pure core on plain values, for the native game and the Enhanced renderer;
//  - an adapter `name(host::Cpu&)` that runs the core against the emulated machine and reproduces the
//    original's registers, FLAGS and memory writes (registered in natives.cpp).
//
// All arithmetic is 16-bit two's complement with wraparound, as the original's. Q15: 7FFFh ~ 1.0.
// Camera space: X right, Y down, Z forward (depth).

#include <array>
#include <cstdint>

namespace vette::host {
class Cpu;
}

namespace vette::game {

struct Vec3 {
    int16_t x, y, z;
    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};
using Mat3 = std::array<int16_t, 9>;  // 3x3, row-major, Q15
struct SinCos {
    int16_t sin, cos;  // Q15, as sincos_deg returns them
};

// --- Pure cores -----------------------------------------------------------------------------------

constexpr int16_t wrap_add(int16_t a, int16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a + b)); }
constexpr int16_t wrap_sub(int16_t a, int16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)); }
constexpr int16_t wrap_neg(int16_t a) { return static_cast<int16_t>(static_cast<uint16_t>(-a)); }  // -8000h = 8000h

// Q15 product the way the original forms it: IMUL, SHL AX,1 / RCL DX,1, keep DX. That is bits 30..15
// of the 32-bit product, truncated (rounded toward -infinity); 8000h * 8000h wraps to 8000h.
constexpr int16_t q15_mul(int16_t a, int16_t b) {
    const auto p = static_cast<uint32_t>(int32_t{a} * b);
    return static_cast<int16_t>(static_cast<uint16_t>(p >> 15));
}

// The matrix's two-term entries (e.g. 3F4B-3F7C): a triple product rounded in two steps,
// q15_mul(a, b) * c, plus d * e, summed in 32 bits, then doubled and the high word kept. When the
// doubling overflows (JNO not taken) the entry saturates to 7FFFh, or 8000h for a negative sum.
constexpr int16_t q15_triple_plus(int16_t a, int16_t b, int16_t c, int16_t d, int16_t e) {
    const uint32_t sum = static_cast<uint32_t>(int32_t{q15_mul(a, b)} * c) + static_cast<uint32_t>(int32_t{d} * e);
    if (((sum >> 31) ^ (sum >> 30)) & 1) {
        return (sum >> 31) ? INT16_MIN : INT16_MAX;
    }
    return static_cast<int16_t>(static_cast<uint16_t>(sum >> 15));
}

// camera_matrix_from_angles: the camera matrix from the sin/cos of the NEGATED yaw (a), pitch (b) and
// roll (c), as the original computes them (DS:3A4A..3A55):
//   | sa*sb*sc + ca*cc    sa*sb*cc + ca*sc    -sa*cb |
//   | -cb*sc              cb*cc                sb    |
//   | ca*sb*sc + sa*cc   -ca*sb*cc + sa*sc     ca*cb |
Mat3 camera_matrix(SinCos a, SinCos b, SinCos c);

// One component of v * M (vec_mul_mat3, 3D59-3D7C): v.x*c0 + v.y*c1 + v.z*c2 summed in 32 bits
// (wrapping), doubled, high word. (c0, c1, c2) is a column of M.
constexpr int16_t q15_dot3(Vec3 v, int16_t c0, int16_t c1, int16_t c2) {
    const uint32_t sum = static_cast<uint32_t>(int32_t{v.x} * c0) + static_cast<uint32_t>(int32_t{v.y} * c1) +
                         static_cast<uint32_t>(int32_t{v.z} * c2);
    return static_cast<int16_t>(static_cast<uint16_t>(sum >> 15));
}

// vec_mul_mat3: the row vector v times M.
constexpr Vec3 vec_mul_mat3(Vec3 v, const Mat3& m) {
    return {q15_dot3(v, m[0], m[3], m[6]), q15_dot3(v, m[1], m[4], m[7]), q15_dot3(v, m[2], m[5], m[8])};
}

// points_rel_camera / world_to_camera_point: a world point relative to the camera position, in
// camera axes: (dy, -dz, dx), d = p - cam.
constexpr Vec3 camera_delta(Vec3 p, Vec3 cam) {
    return {wrap_sub(p.y, cam.y), wrap_neg(wrap_sub(p.z, cam.z)), wrap_sub(p.x, cam.x)};
}

// world_to_camera_point: a world point in camera space.
constexpr Vec3 world_to_camera(Vec3 p, Vec3 cam, const Mat3& m) { return vec_mul_mat3(camera_delta(p, cam), m); }

// build_axis_table: k * axis for k = 1, 2, 3, 5, 7, 8, 9, 11, built by repeated wrapping addition of
// a step that is doubled, halved (SAR, so an earlier wrap is not undone) and doubled again.
constexpr std::array<Vec3, 8> axis_multiples(Vec3 axis) {
    const auto twice = [](Vec3 s) { return Vec3{wrap_add(s.x, s.x), wrap_add(s.y, s.y), wrap_add(s.z, s.z)}; };
    const auto half = [](Vec3 s) {
        return Vec3{static_cast<int16_t>(s.x >> 1), static_cast<int16_t>(s.y >> 1), static_cast<int16_t>(s.z >> 1)};
    };
    std::array<Vec3, 8> out{};
    out[0] = axis;
    Vec3 acc = axis, step = axis;
    size_t n = 1;
    const auto advance = [&](int times) {
        for (int i = 0; i < times; ++i) {
            acc = {wrap_add(acc.x, step.x), wrap_add(acc.y, step.y), wrap_add(acc.z, step.z)};
            out[n++] = acc;
        }
    };
    advance(2);  // 2, 3
    step = twice(step);
    advance(2);  // 5, 7
    step = half(step);
    advance(2);  // 8, 9
    step = twice(step);
    advance(1);  // 11
    return out;
}

// --- Adapters (emulated machine) ------------------------------------------------------------------

// sincos_deg (3009:4E3C). In: AX = angle in degrees, DS:BX = output. Out: [BX] = sin, [BX+2] = cos
// (Q15, 7FFFh ~ 1.0), read from the 91-entry cos table at DS:38DE. AX and FLAGS as the original
// leaves them.
void sincos_deg(host::Cpu& cpu);

// camera_matrix_from_angles (3009:3F2D). In: DS:SI -> yaw, pitch, roll (degrees). Out: sin/cos of
// the negated angles at DS:3A4A..3A55, the matrix at ES:32B1 (STOSW), SI += 6.
void camera_matrix_from_angles(host::Cpu& cpu);

// vec_mul_mat3 (3009:3D51). In: DS:SI -> vector, DS:BX -> matrix, ES:DI -> output. CX preserved.
void vec_mul_mat3(host::Cpu& cpu);

// points_rel_camera (3009:3D8C). In: DS:SI -> CX world points {x, y, z}, ES:DI -> output.
// Camera position at DS:2C71/2C73/2C75.
void points_rel_camera(host::Cpu& cpu);

// xform_points_to_camera (3009:3D2F). In: DS:BX -> AX world points. Out: deltas at ES:266E, camera
// space at ES:2286 (matrix DS:32B1). SI preserved.
void xform_points_to_camera(host::Cpu& cpu);

// world_to_camera_point (3009:3917). In: DS:SI -> one world point. Out: delta at ES:2286, camera
// space at ES:344E.
void world_to_camera_point(host::Cpu& cpu);

// build_axis_table (3009:39B9). In: DS:SI -> 3 axis vectors, ES:DI -> 24 output vectors. Leaves
// DS = ES.
void build_axis_table(host::Cpu& cpu);

} // namespace vette::game
