// Pure cores of the ported 3D math (src/game/math3d.h, projection.h), with values worked out by hand
// from the original's instruction sequences. The adapters are proven against the original by the
// vette_run --verify harness; these tests pin down the cores' arithmetic, quirks included.

#include "game/math3d.h"
#include "game/projection.h"
#include "test.h"

using namespace vette::game;

namespace {

constexpr int16_t kOne = 0x7FFF;  // Q15 ~1.0

} // namespace

// The constexpr cores are checked at compile time.
static_assert(q15_mul(kOne, kOne) == 0x7FFE);
static_assert(q15_mul(0x4000, 0x4000) == 0x2000);
static_assert(q15_mul(0x4000, -0x4000) == -0x2000);
static_assert(q15_mul(-1, 1) == -1);                      // truncated toward -infinity
static_assert(q15_mul(INT16_MIN, INT16_MIN) == INT16_MIN);  // the doubling wraps

// (7FFE * 7FFF = 3FFE8002) + 0, doubled: 7FFD.
static_assert(q15_triple_plus(kOne, kOne, kOne, 0, 0) == 0x7FFD);
// 3FFE8002 + 3FFF0001 = 7FFD8003: the doubling overflows, positive.
static_assert(q15_triple_plus(kOne, kOne, kOne, kOne, kOne) == INT16_MAX);
// C0010000 + C0008000 = 80018000: overflows, negative.
static_assert(q15_triple_plus(kOne, kOne, INT16_MIN, INT16_MIN, kOne) == INT16_MIN);

// The 32-bit dot product wraps: 3 * 3FFF0001 = BFFD0003, doubled high word 7FFA.
static_assert(q15_dot3({kOne, kOne, kOne}, kOne, kOne, kOne) == 0x7FFA);
static_assert(vec_mul_mat3({100, -200, 300}, {0x7FFE, 0, 0, 0, 0x7FFE, 0, 0, 0, 0x7FFE}) == Vec3{99, -200, 299});

static_assert(camera_delta({1000, 2000, 300}, {900, 2100, 100}) == Vec3{-100, -200, 100});
static_assert(camera_delta({0, INT16_MAX, 0}, {0, -1, 0}) == Vec3{INT16_MIN, 0, 0});  // 16-bit wrap
static_assert(world_to_camera({1000, 2000, 300}, {900, 2100, 100}, {0x7FFE, 0, 0, 0, 0x7FFE, 0, 0, 0, 0x7FFE}) ==
              Vec3{-100, -200, 99});

TEST(game_camera_matrix) {
    const SinCos zero{0, kOne};
    const Mat3 identity = camera_matrix(zero, zero, zero);
    const Mat3 expect_identity{0x7FFE, 0, 0, 0, 0x7FFE, 0, 0, 0, 0x7FFE};
    CHECK(identity == expect_identity);

    // Yaw 90: the original passes sincos(-90) = (-7FFF, 0). The depth column then picks up dy
    // (yaw 90 faces +y), and -sa*ca rounds down to 8001h.
    const Mat3 yaw90 = camera_matrix({-kOne, 0}, zero, zero);
    const Mat3 expect_yaw90{0, 0, 0x7FFE, 0, 0x7FFE, 0, -0x7FFF, 0, 0};
    CHECK(yaw90 == expect_yaw90);
}

TEST(game_axis_multiples) {
    const auto k = axis_multiples({1024, -3, 0x7000});
    static constexpr int kMul[8] = {1, 2, 3, 5, 7, 8, 9, 11};
    for (size_t i = 0; i < 8; ++i) {
        CHECK_EQ(k[i].x, 1024 * kMul[i]);
        CHECK_EQ(k[i].y, -3 * kMul[i]);
    }
    // 7000h: the doubled step wraps to E000h, and SAR then gives F000h rather than 7000h.
    static constexpr uint16_t kZ[8] = {0x7000, 0xE000, 0x5000, 0x3000, 0x1000, 0x0000, 0xF000, 0xD000};
    for (size_t i = 0; i < 8; ++i) {
        CHECK_EQ(static_cast<uint16_t>(k[i].z), kZ[i]);
    }
}

TEST(game_project_coord_narrow) {
    ScreenCoord c = project_coord(100, 50, 160, ScreenAxis::X);
    CHECK_EQ(c.wide, 672);
    CHECK_EQ(c.narrow, 672);
    CHECK(c.narrow_stored && !c.overflow);

    c = project_coord(-100, 50, 160, ScreenAxis::X);
    CHECK_EQ(c.wide, -352);
    CHECK_EQ(c.narrow, -352);

    c = project_coord(-1, 3, 0, ScreenAxis::Y);  // IDIV truncates toward zero: -256/3 = -85
    CHECK_EQ(c.wide, -85);

    // 25600 + 7FF0h overflows 16 bits (JO): flag 1, the wrapped word is still stored, and the
    // 32-bit copy is right.
    c = project_coord(100, 1, 0x7FF0, ScreenAxis::X);
    CHECK(c.narrow_stored && c.overflow);
    CHECK_EQ(static_cast<uint16_t>(c.narrow), 0xE3F0);
    CHECK_EQ(c.wide, 25600 + 0x7FF0);
    // ADC DX,0 treats the centre as unsigned, so a negative centre gives a wrong 32-bit value.
    c = project_coord(-100, 1, INT16_MIN, ScreenAxis::X);
    CHECK_EQ(c.wide, 0x1C00);
}

TEST(game_project_coord_wide) {
    // IDIV overflow -> INT 0 -> exact 32-bit recompute.
    ScreenCoord c = project_coord(0x4000, 1, 160, ScreenAxis::X);
    CHECK(!c.narrow_stored && c.overflow);
    CHECK_EQ(c.wide, 0x400000 + 160);
    CHECK_EQ(project_coord(0x4000, 3, 160, ScreenAxis::X).wide, 1398101 + 160);
    CHECK_EQ(project_coord(-0x4000, 1, 160, ScreenAxis::Y).wide, -0x400000 + 160);

    // 80h * 256 = 8000h: the IDIV overflows; for x the recompute is exact, but for y the CWD of the
    // low word 8000h makes the first DIV fault too, and the handler's 7FFFh is used as the quotient.
    CHECK_EQ(project_coord(0x80, 1, 100, ScreenAxis::X).wide, 0x8000 + 100);
    CHECK_EQ(project_coord(0x80, 1, 100, ScreenAxis::Y).wide, 0x7FFF + 100);

    // A genuine quotient of 7FFFh (7FFFh * 256 / 256) also takes the recompute, with DX holding the
    // remainder (0) instead of the dividend's high word (7Fh): only the low word's 255 survives.
    c = project_coord(0x7FFF, 256, 160, ScreenAxis::X);
    CHECK(!c.narrow_stored && c.overflow);
    CHECK_EQ(c.wide, 255 + 160);
}
