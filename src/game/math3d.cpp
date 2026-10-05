#include "game/math3d.h"

#include <cstddef>
#include <initializer_list>

#include "game/x86.h"

namespace vette::game {

using host::AX;
using host::BP;
using host::BX;
using host::CX;
using host::DI;
using host::DS;
using host::DX;
using host::ES;
using host::SI;
using host::SP;

namespace {

// Game variables in the data segment (notes 03).
constexpr uint16_t kCamX = 0x2C71, kCamY = 0x2C73, kCamZ = 0x2C75;  // camera position
constexpr uint16_t kCamMatrix = 0x32B1;                               // camera matrix (Q15)
constexpr uint16_t kSinCosYaw = 0x3A4A, kSinCosPitch = 0x3A4E, kSinCosRoll = 0x3A52;
constexpr uint16_t kDeltaBuf = 0x266E;   // xform_points_to_camera: camera-relative deltas
constexpr uint16_t kCameraBuf = 0x2286;  // camera-space points
constexpr uint16_t kCameraPoint = 0x344E;  // world_to_camera_point's result

uint16_t u(int16_t v) { return static_cast<uint16_t>(v); }
int16_t s(uint16_t v) { return static_cast<int16_t>(v); }

// The body of vec_mul_mat3 up to its RET, on the machine state. SP points at the return address.
// Per column: LODSW x3 and the column at [BX], [BX+6], [BX+12] (all DS), one STOSW; then BX += 2
// (ADD BX,6 x3, SUB BX,10h) and SUB SI,6 (so SI is back where it started when DF = 0).
void vec_mul_mat3_body(Registers& r, Memory& m) {
    const uint16_t ds = r.s[DS];
    const auto frame = static_cast<uint16_t>(r.r[SP] - 6);  // PUSH CX; SUB SP,4; MOV BP,SP
    uint16_t last = 0, last_hi = 0;
    for (int col = 0; col < 3; ++col) {
        Vec3 v{};
        v.x = s(lodsw(r, m));
        v.y = s(lodsw(r, m));
        v.z = s(lodsw(r, m));
        const uint16_t bx = r.r[BX];
        const int16_t c0 = s(rd16(m, ds, bx));
        const int16_t c1 = s(rd16(m, ds, off16(bx, 6)));
        const int16_t c2 = s(rd16(m, ds, off16(bx, 12)));
        last = u(q15_dot3(v, c0, c1, c2));
        stosw(r, m, last);
        last_hi = static_cast<uint16_t>(static_cast<uint32_t>(int32_t{v.z} * c2) >> 16);  // DX of the last IMUL
        r.r[BX] = off16(bx, 2);
        r.r[SI] = off16(r.r[SI], -6);
    }
    // AX = the last component (STOSW keeps it), CX restored by POP CX, FLAGS from ADD SP,4.
    r.r[AX] = last;
    r.r[DX] = last_hi;
    r.r[BP] = frame;
    flags::add16(r, frame, 4);
}

// The body of points_rel_camera up to its RET: for CX points (CX = 0 means 65536, as LOOP counts),
// LODSW x, y, z and STOSW y - camY, -(z - camZ), x - camX. The camera words are re-read for every
// component, between the stores, as the original does.
void points_rel_camera_body(Registers& r, Memory& m) {
    const uint16_t ds = r.s[DS];
    do {
        const int16_t x = s(lodsw(r, m));
        const int16_t y = s(lodsw(r, m));
        const int16_t z = s(lodsw(r, m));
        r.r[BX] = u(x);
        r.r[DX] = u(z);
        stosw(r, m, u(wrap_sub(y, s(rd16(m, ds, kCamY)))));
        stosw(r, m, u(wrap_neg(wrap_sub(z, s(rd16(m, ds, kCamZ))))));
        const uint16_t cam_x = rd16(m, ds, kCamX);
        r.r[AX] = u(wrap_sub(x, s(cam_x)));
        flags::sub16(r, u(x), cam_x);  // the last flag operation: SUB AX,[2C71]
        stosw(r, m, r.r[AX]);
        r.r[CX] = off16(r.r[CX], -1);
    } while (r.r[CX] != 0);
}

// A near CALL from inside a ported routine: the return address goes below SP, which is dead stack
// (not compared), so only SP moves.
template <typename Body>
void near_call(Registers& r, Body body) {
    r.r[SP] = off16(r.r[SP], -2);
    body();
    r.r[SP] = off16(r.r[SP], 2);
}

} // namespace

// --- Pure cores -----------------------------------------------------------------------------------

Mat3 camera_matrix(SinCos a, SinCos b, SinCos c) {
    // In the original's order (3F48-405C); each entry reads its operands from DS:3A4A..3A55.
    return {
        q15_triple_plus(a.sin, b.sin, c.sin, a.cos, c.cos),
        q15_triple_plus(a.sin, b.sin, c.cos, a.cos, c.sin),
        q15_mul(wrap_neg(a.sin), b.cos),
        q15_mul(wrap_neg(b.cos), c.sin),
        q15_mul(b.cos, c.cos),
        b.sin,
        q15_triple_plus(a.cos, b.sin, c.sin, a.sin, c.cos),
        q15_triple_plus(wrap_neg(a.cos), b.sin, c.cos, a.sin, c.sin),
        q15_mul(a.cos, b.cos),
    };
}

// --- Adapters -------------------------------------------------------------------------------------

void sincos_deg(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    const uint16_t ds = r.s[DS];
    constexpr uint16_t kCosTable = 0x38DE;  // cos(0..90 degrees), Q15
    // The original indexes with 16-bit address arithmetic, so angles outside -360..359 read past
    // the table exactly as it does.
    auto entry = [&](uint16_t index) { return rd16(m, ds, static_cast<uint16_t>(kCosTable + (index << 1))); };

    // Negative angles wrap once; every comparison below is signed (JGE/JLE).
    uint16_t a = r.r[AX];
    if (static_cast<int16_t>(a) < 0) {
        a = static_cast<uint16_t>(a + 360);
    }
    const auto quadrant_angle = [&](int base) { return static_cast<uint16_t>(a - base); };
    // Each quadrant reads one entry (MOV DI,[DI]), stores it, then reads the other (LODSW) and stores
    // that. The order matters when an out-of-range angle makes the second read land on the first
    // store, so it is kept: q1 and q3 store sin first, q2 and q4 store cos first.
    const uint16_t bx = r.r[BX];
    const auto store_sin = [&](uint16_t v) { wr16(m, ds, bx, v); };
    const auto store_cos = [&](uint16_t v) { wr16(m, ds, static_cast<uint16_t>(bx + 2), v); };
    if (static_cast<int16_t>(a) <= 90) {
        store_sin(entry(static_cast<uint16_t>(90 - a)));
        const uint16_t cos = entry(a);
        store_cos(cos);
        r.r[AX] = cos;
        flags::add16(r, kCosTable, static_cast<uint16_t>(a << 1));  // last flag op: ADD SI,AX
    } else if (static_cast<int16_t>(a) <= 180) {
        const uint16_t b = quadrant_angle(90);
        store_cos(static_cast<uint16_t>(-entry(static_cast<uint16_t>(90 - b))));
        const uint16_t sin = entry(b);
        store_sin(sin);
        r.r[AX] = sin;
        flags::add16(r, kCosTable, static_cast<uint16_t>(b << 1));
    } else if (static_cast<int16_t>(a) <= 270) {
        const uint16_t c = quadrant_angle(180);
        store_sin(static_cast<uint16_t>(-entry(static_cast<uint16_t>(90 - c))));
        const uint16_t t = entry(c);
        r.r[AX] = static_cast<uint16_t>(-t);  // cos
        store_cos(r.r[AX]);
        flags::neg16(r, t);  // last flag op: NEG AX
    } else {
        const uint16_t d = quadrant_angle(270);
        store_cos(entry(static_cast<uint16_t>(90 - d)));
        const uint16_t t = entry(d);
        r.r[AX] = static_cast<uint16_t>(-t);  // sin
        store_sin(r.r[AX]);
        flags::neg16(r, t);
    }
}

void camera_matrix_from_angles(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    const uint16_t ds = r.s[DS];

    // LODSW; NEG AX; MOV BX,out; CALL sincos_deg, for yaw, pitch and roll.
    for (const uint16_t out : {kSinCosYaw, kSinCosPitch, kSinCosRoll}) {
        r.r[AX] = u(wrap_neg(s(lodsw(r, m))));
        r.r[BX] = out;
        near_call(r, [&] { sincos_deg(cpu); });
    }
    const auto sincos = [&](uint16_t at) { return SinCos{s(rd16(m, ds, at)), s(rd16(m, ds, off16(at, 2)))}; };
    const SinCos a = sincos(kSinCosYaw), b = sincos(kSinCosPitch), c = sincos(kSinCosRoll);

    // The original re-reads the sin/cos words for every entry, between its stores. The stores
    // (ES:32B1 +-18 bytes) could only reach DS:3A4A..3A55 with ES about 7Ah paragraphs above DS; the
    // callers have ES = DS, so computing the whole matrix before storing it is equivalent.
    r.r[DI] = kCamMatrix;
    for (const int16_t e : camera_matrix(a, b, c)) {
        stosw(r, m, u(e));
    }

    // Register residue. CX:BX hold the first triple product of entry 7 (402C/402E). Entry 8 is
    // IMUL; SHL AX,1; RCL DX,1; MOV AX,DX: those two shifts set the final FLAGS.
    const auto p7 = static_cast<uint32_t>(int32_t{q15_mul(wrap_neg(a.cos), b.sin)} * c.cos);
    r.r[CX] = static_cast<uint16_t>(p7);
    r.r[BX] = static_cast<uint16_t>(p7 >> 16);
    const auto p8 = static_cast<uint32_t>(int32_t{a.cos} * b.cos);
    flags::shl1(r, static_cast<uint16_t>(p8));
    flags::rcl1(r, static_cast<uint16_t>(p8 >> 16));
    r.r[AX] = r.r[DX] = u(q15_mul(a.cos, b.cos));
}

void vec_mul_mat3(Cpu& cpu) { vec_mul_mat3_body(cpu.regs, cpu.memory()); }

void points_rel_camera(Cpu& cpu) { points_rel_camera_body(cpu.regs, cpu.memory()); }

void xform_points_to_camera(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    const uint16_t saved_si = r.r[SI];
    r.r[SP] = off16(r.r[SP], -2);  // PUSH SI
    r.r[SI] = r.r[BX];
    r.r[DI] = kDeltaBuf;
    r.r[CX] = r.r[AX];
    r.r[BP] = r.r[AX];
    near_call(r, [&] { points_rel_camera_body(r, m); });
    r.r[CX] = r.r[BP];
    r.r[SI] = kDeltaBuf;
    r.r[DI] = kCameraBuf;
    do {
        r.r[BX] = kCamMatrix;
        near_call(r, [&] { vec_mul_mat3_body(r, m); });
        flags::add16(r, r.r[SI], 6);  // ADD SI,6: the last flag operation
        r.r[SI] = off16(r.r[SI], 6);
        r.r[CX] = off16(r.r[CX], -1);
    } while (r.r[CX] != 0);
    r.r[SI] = saved_si;  // POP SI
    r.r[SP] = off16(r.r[SP], 2);
}

void world_to_camera_point(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    const uint16_t ds = r.s[DS];
    const uint16_t si = r.r[SI];
    r.r[DI] = kCameraBuf;
    // In the original's order: [SI+2] and [SI+4] by MOV, then [SI] by LODSW.
    const int16_t y = s(rd16(m, ds, off16(si, 2)));
    stosw(r, m, u(wrap_sub(y, s(rd16(m, ds, kCamY)))));
    const int16_t z = s(rd16(m, ds, off16(si, 4)));
    stosw(r, m, u(wrap_neg(wrap_sub(z, s(rd16(m, ds, kCamZ))))));
    const int16_t x = s(lodsw(r, m));
    stosw(r, m, u(wrap_sub(x, s(rd16(m, ds, kCamX)))));
    r.r[SI] = kCameraBuf;
    r.r[DI] = kCameraPoint;
    r.r[BX] = kCamMatrix;
    near_call(r, [&] { vec_mul_mat3_body(r, m); });
}

void build_axis_table(Cpu& cpu) {
    Registers& r = cpu.regs;
    Memory& m = cpu.memory();
    // SUB SP,6; MOV BP,SP: the original keeps the x step and the y and z sums in this stack frame,
    // the native code in locals (only a store from ES:DI into the stack itself would tell them apart).
    const auto frame = static_cast<uint16_t>(r.r[SP] - 6);
    Vec3 axis{};
    std::array<Vec3, 8> k{};
    for (int i = 0; i < 3; ++i) {
        // LODSW / STOSW for each component in turn. The first axis is read from DS; the routine then
        // sets DS = ES (MOV AX,ES; MOV DS,AX), so the other two are read from ES.
        axis.x = s(lodsw(r, m));
        stosw(r, m, u(axis.x));
        axis.y = s(lodsw(r, m));
        stosw(r, m, u(axis.y));
        axis.z = s(lodsw(r, m));
        stosw(r, m, u(axis.z));
        k = axis_multiples(axis);
        for (size_t j = 1; j < k.size(); ++j) {
            stosw(r, m, u(k[j].x));
            stosw(r, m, u(k[j].y));
            stosw(r, m, u(k[j].z));
        }
        r.s[DS] = r.s[ES];
    }
    // Register residue: BX accumulates x (the last is 11 * x), DX holds the y step (y, doubled,
    // halved by SAR, doubled), AX = ES, CL and CH have counted down to 0. FLAGS from ADD SP,6.
    const auto twice = [](uint16_t v) { return static_cast<uint16_t>(v << 1); };
    r.r[AX] = r.s[ES];
    r.r[BX] = u(k[7].x);
    r.r[CX] = 0;
    r.r[DX] = twice(u(static_cast<int16_t>(s(twice(u(axis.y))) >> 1)));
    r.r[BP] = frame;
    flags::add16(r, frame, 6);
}

} // namespace vette::game
