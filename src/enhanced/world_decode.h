#pragma once
// Pure decoders for the original's world data (re/notes/05-world-data.md): packed vertex blocks, the
// segment-245A models, the map grid and the cell-type lists. They read a 1 MB memory image of the
// machine (VETTE.EXE loaded at segment 1000h and unpacked); nothing here runs emulated code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "enhanced/world.h"

namespace vette::enhanced {

// Read-only view of a 1 MB real-mode memory image. Offsets wrap within their 64 KB segment.
struct ImageView {
    const uint8_t* ram = nullptr;
    uint8_t u8(uint16_t seg, uint16_t off) const { return ram[((uint32_t{seg} << 4) + off) & 0xFFFFF]; }
    uint16_t u16(uint16_t seg, uint16_t off) const {
        return static_cast<uint16_t>(u8(seg, off) | u8(seg, static_cast<uint16_t>(off + 1)) << 8);
    }
    int16_t s16(uint16_t seg, uint16_t off) const { return static_cast<int16_t>(u16(seg, off)); }
};

// --- Packed vertex blocks (3009:3879 -> 3D2F origin, 3A64 deltas) ---------------------------------

using V3s = std::array<int16_t, 3>;

// The vectors the packed words select from: DS:3184, k * axis for k = 1, 2, 3, 5, 7, 8, 9, 11 and the
// three model axes (8 per axis), and the byte offsets DS:3181..3183 that pick each word's group.
struct AxisTable {
    std::array<V3s, 24> vectors{};
    std::array<uint8_t, 3> base{0x00, 0x30, 0x60};  // for packed word 0, 1, 2 (DS:3183, 3182, 3181)
};

// The table for an unrotated camera with exact unit axes, in world component order (x north, y east,
// z up): axis 0 = +y (east), axis 1 = +z (up), axis 2 = +x (north), each 1024 long. Decoding with it
// gives object-local world coordinates. (The original's own table, from the Q15 camera matrix, has
// axes of 1023.97 that truncate to 1023 when the camera looks along an axis.)
AxisTable ideal_axis_table();

// SAR with the original's rounding (SAR then ADC 0: add the last bit shifted out).
constexpr int16_t sar_round(int16_t v, unsigned shift) {
    shift &= 31;  // the 286 masks shift counts to 5 bits
    if (shift == 0) {
        return v;
    }
    const unsigned s = shift > 15 ? 15 : shift;
    const unsigned c = shift - 1 > 15 ? 15 : shift - 1;
    const int carry = (v >> c) & 1;
    return static_cast<int16_t>(static_cast<uint16_t>((v >> s) + carry));
}

// unpack_vertices (3009:3A64) for one block: vertex 0 = origin, then `count` vertices, each the
// previous one plus the deltas its three packed words select (16-bit wrapping sums, as the original).
// A word is 0 (no delta) or hi = right shift, lo = signed selector; the vector is read at
// 317Eh + base[word] + |lo|. Returns false if a selector falls outside the table.
bool unpack_vertices(const V3s& origin, const int16_t* words, size_t count, const AxisTable& table,
                     std::vector<V3s>& out);

// --- Segment-245A models (3009:B9F6 / B765) ---------------------------------------------------------

// Decodes the mesh whose header is at seg:header. `colour_override` >= 0 replaces the colour word of
// the faces at the addresses in `patched_faces` (the far box's colour patch, 3009:BA18).
bool decode_model_mesh(const ImageView& img, uint16_t seg, uint16_t header, ModelMesh& out, std::string& error,
                       int colour_override = -1, const std::vector<uint16_t>& patched_faces = {});

// --- Cell-type lists ---------------------------------------------------------------------------------

// A list of {w routine, w dx, w dy, w dz} ... FFFF at ds:at. Returns the offset after the terminator,
// or 0 if no terminator was found within `max_entries`.
uint16_t read_entry_list(const ImageView& img, uint16_t ds, uint16_t at, std::vector<ListEntry>& out,
                         size_t max_entries = 64);

} // namespace vette::enhanced
