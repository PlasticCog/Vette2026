#include "graphics/draw_tracker.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "graphics/game_state.h"

namespace vette::graphics {

namespace {

using host::AX;
using host::BP;
using host::BX;
using host::CX;
using host::Cpu;
using host::DI;
using host::DS;
using host::DX;
using host::ES;
using host::SI;
using host::SS;
using host::SP;

constexpr std::size_t kVram = 0x10000;  // A0000h-AFFFFh

// Entry points in the game's code segment (image 3009h).
constexpr std::uint16_t kText88EB = 0x88EB;   // 8x10 text, 640 mode: DS:SI zero-terminated, DI, AH colour
constexpr std::uint16_t kTextF3E1 = 0xF3E1;   // 8x10 text (menus): count-prefixed, BX row, CX column
constexpr std::uint16_t kText5D55 = 0x5D55;   // 8x7 text, 320 mode: DX page, DI, AH colour
constexpr std::uint16_t kSprite858F = 0x858F; // masked sprite row writer (mask pass from 3009:854C)
constexpr std::uint16_t kUnpack8666 = 0x8666; // RLE picture into a page: AX seg, DI, BH bytes, BP rows
constexpr std::uint16_t kCopy8820 = 0x8820;   // page copy (latches): AX:SI -> DX:DI, BH x BP
constexpr std::uint16_t kFill88AF = 0x88AF;   // solid rectangle: AX:DI, BH x BP, BL colour (set/reset)
constexpr std::uint16_t kXor886E = 0x886E;    // XOR rectangle (highlight bars): AX:DI, BH x BP, BL
constexpr std::uint16_t kMode0E = 0x8F00, kMode0D = 0x8F25;  // mode sets (clear video memory)
// Byte-run copies and fills: A800:SI -> draw page:DI, CX bytes (the title restoring its credits area);
// draw page:SI -> display page:DI, CX bytes; A800:SI -> draw page:1F40, 1F40h bytes; A000:DI, CX
// bytes filled; A000:DI, BL bytes x BH rows filled (rows [1ACB:7763] apart).
constexpr std::uint16_t kCopyCD22 = 0xCD22, kCopyC9A5 = 0xC9A5, kCopy884B = 0x884B;
constexpr std::uint16_t kFillF318 = 0xF318, kFillE600 = 0xE600;
constexpr std::uint16_t kFontSeg8x10 = 0x224A, kFontSeg8x7 = 0x3243;  // running program's segments
// The screen transition (only the title calls it): from page BX onto page AX on display, in the way
// DS:E01C picks (first a dissolve, 8239h: one byte x 5 rows at a time, copied at 8285h; later slides
// and scrolls); every way ends at 82B1h, also when a key cuts it short.
constexpr std::uint16_t kTransition = 0x7FE7, kDissolveBlock = 0x8285, kTransitionEnd = 0x82B1;

std::uint32_t vram(std::uint16_t seg, std::uint16_t off) {
    // A segment:offset as a video memory byte address, or kVram when it isn't video memory.
    const std::uint32_t linear = (static_cast<std::uint32_t>(seg) << 4) + off;
    return linear >= 0xA0000 && linear < 0xB0000 ? linear - 0xA0000 : static_cast<std::uint32_t>(kVram);
}

}  // namespace

DrawTracker::DrawTracker(host::Machine& machine)
    : machine_(machine), shadow_(kVram * 8, kNone), cells_(kVram, 0), pages_(1, 0), revealed_(0x4000, 0) {
    Cpu& cpu = machine.cpu();
    const auto at = [](std::uint16_t off) { return kGameCs + off; };
    const std::uint8_t* ram = machine.memory().ram();
    const auto cs8 = [ram](std::uint16_t off) { return ram[kGameCs + off]; };
    // 640-mode rows are 80 bytes; cs:926B adds 28h to 28h there (0 in 320 mode), cs:926A shifts by 1.
    const auto stride_b = [cs8] { return 0x28 + cs8(0x926B); };

    watches_.push_back(cpu.add_watch(at(kText88EB), [this](Cpu& c) { text_88eb(c); }));
    watches_.push_back(cpu.add_watch(at(kTextF3E1), [this](Cpu& c) { text_f3e1(c); }));
    watches_.push_back(cpu.add_watch(at(kText5D55), [this](Cpu& c) { text_5d55(c); }));
    watches_.push_back(cpu.add_watch(at(kSprite858F), [this](Cpu& c) { sprite_mask(c); }));
    watches_.push_back(cpu.add_watch(at(kUnpack8666), [this](Cpu& c) {
        const auto& r = c.regs;
        clear({vram(r.r[AX], r.r[DI]), r.hi(BX), r.r[BP], 0x50}, "unpack");
    }));
    watches_.push_back(cpu.add_watch(at(kCopy8820), [this, stride_b](Cpu& c) {
        const auto& r = c.regs;
        const int stride = stride_b();
        copy({vram(r.r[AX], r.r[SI]), r.hi(BX), r.r[BP], stride}, {vram(r.r[DX], r.r[DI]), r.hi(BX), r.r[BP], stride}, true);
    }));
    watches_.push_back(cpu.add_watch(at(kFill88AF), [this, stride_b](Cpu& c) {
        const auto& r = c.regs;
        fill({vram(r.r[AX], r.r[DI]), r.hi(BX), r.r[BP], stride_b()}, r.lo(BX));
    }));
    watches_.push_back(cpu.add_watch(at(kXor886E), [this, cs8](Cpu& c) {
        const auto& r = c.regs;
        xor_colour({vram(r.r[AX], r.r[DI]), r.hi(BX), r.r[BP], 0x50 >> (cs8(0x926A) & 7)}, r.lo(BX));
    }));
    const auto cs16 = [ram](std::uint16_t off) {
        return static_cast<std::uint16_t>(ram[kGameCs + off] | ram[kGameCs + off + 1] << 8);
    };
    const auto run = [](std::uint32_t from, int bytes) { return Rect{from, bytes, 1, bytes}; };
    watches_.push_back(cpu.add_watch(at(kCopyCD22), [this, cs16, run](Cpu& c) {
        const auto& r = c.regs;
        copy(run(vram(0xA800, r.r[SI]), r.r[CX]), run(vram(cs16(0x8DAB), r.r[DI]), r.r[CX]), true);
    }));
    watches_.push_back(cpu.add_watch(at(kCopyC9A5), [this, cs16, run](Cpu& c) {
        const auto& r = c.regs;
        copy(run(vram(cs16(0x8DAB), r.r[SI]), r.r[CX]), run(vram(cs16(0x8DA9), r.r[DI]), r.r[CX]), true);
    }));
    watches_.push_back(cpu.add_watch(at(kCopy884B), [this, cs16, run](Cpu& c) {
        copy(run(vram(0xA800, c.regs.r[SI]), 0x1F40), run(vram(cs16(0x8DAB), 0x1F40), 0x1F40), true);
    }));
    watches_.push_back(cpu.add_watch(at(kFillF318), [this, run](Cpu& c) {
        clear(run(vram(0xA000, c.regs.r[DI]), c.regs.r[CX]), "fillF318");
    }));
    watches_.push_back(cpu.add_watch(at(kFillE600), [this, ram](Cpu& c) {
        const auto& r = c.regs;
        clear({vram(0xA000, r.r[DI]), r.lo(BX), r.hi(BX), ram[Cpu::linear(0x1ACB, 0x7763)]}, "fillE600");
    }));
    watches_.push_back(cpu.add_watch(at(kTransition), [this](Cpu& c) {
        transition_ = vram(c.regs.r[AX], 0);
        std::fill(revealed_.begin(), revealed_.end(), std::uint8_t{0});
        pages_[0] = 1;
        log("transition start");
    }));
    watches_.push_back(cpu.add_watch(at(kDissolveBlock), [this](Cpu& c) {
        const auto& r = c.regs;
        const Rect from{vram(r.s[DS], r.r[SI]), 1, 5, 0x50}, to{vram(r.s[ES], r.r[SI]), 1, 5, 0x50};
        copy(from, to, false);
        if (transition_ >= kVram) return;
        for (int y = 0; y < 5; ++y) {
            const std::uint32_t i = (r.r[SI] + static_cast<std::uint32_t>(y) * 0x50) & 0xFFFF;
            if (i < revealed_.size()) revealed_[i] = 1;
        }
    }));
    watches_.push_back(cpu.add_watch(at(kTransitionEnd), [this](Cpu&) {
        transition_ = static_cast<std::uint32_t>(kVram);
        log("transition end");
    }));
    for (const std::uint16_t mode : {kMode0E, kMode0D})
        watches_.push_back(cpu.add_watch(at(mode), [this](Cpu&) {
            std::fill(shadow_.begin(), shadow_.end(), kNone);
            std::fill(cells_.begin(), cells_.end(), std::uint8_t{0});
            pages_[0] = 0;
            transition_ = static_cast<std::uint32_t>(kVram);
            log("mode set");
        }));
}

DrawTracker::~DrawTracker() {
    for (const auto id : watches_) machine_.cpu().remove_watch(id);
}

void DrawTracker::log(const std::string& line) const {
    if (log_) log_(line);
}

void DrawTracker::clear(const Rect& r, const char* what) {
    if (log_) log(std::string(what) + " " + std::to_string(r.at) + " " + std::to_string(r.bytes) + "x" + std::to_string(r.rows));
    if (r.at >= kVram || r.bytes <= 0 || r.rows <= 0) return;
    for (int y = 0; y < r.rows; ++y)
        for (int b = 0; b < r.bytes; ++b) {
            const std::size_t a = (r.at + static_cast<std::uint32_t>(y * r.stride + b)) & 0xFFFF;
            std::fill_n(shadow_.begin() + static_cast<std::ptrdiff_t>(a * 8), 8, kNone);
            cells_[a] = 0;
        }
}

// A solid rectangle the game draws over a picture (the course map's text panels) is its drawing too.
void DrawTracker::fill(const Rect& r, std::uint8_t colour) {
    if (log_) log("fill88AF " + std::to_string(r.at) + " " + std::to_string(r.bytes) + "x" + std::to_string(r.rows));
    if (r.at >= kVram || r.bytes <= 0 || r.rows <= 0) return;
    pages_[0] = 1;
    for (int y = 0; y < r.rows; ++y)
        for (int b = 0; b < r.bytes; ++b) {
            const std::size_t a = (r.at + static_cast<std::uint32_t>(y * r.stride + b)) & 0xFFFF;
            std::fill_n(shadow_.begin() + static_cast<std::ptrdiff_t>(a * 8), 8, static_cast<std::uint8_t>(colour & 15));
            cells_[a] = 0;
        }
}

void DrawTracker::copy(const Rect& from, const Rect& to, bool logged) {
    if (log_ && logged)
        log("copy " + std::to_string(from.at) + " -> " + std::to_string(to.at) + " " + std::to_string(from.bytes) + "x" +
            std::to_string(from.rows));
    if (from.at >= kVram || to.at >= kVram || from.bytes <= 0 || from.rows <= 0) return;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(from.bytes) * from.rows * 8), cell(static_cast<std::size_t>(from.bytes) * from.rows);
    for (int y = 0; y < from.rows; ++y)
        for (int b = 0; b < from.bytes; ++b) {
            const std::size_t a = (from.at + static_cast<std::uint32_t>(y * from.stride + b)) & 0xFFFF;
            const std::size_t i = static_cast<std::size_t>(y) * from.bytes + b;
            std::copy_n(shadow_.begin() + static_cast<std::ptrdiff_t>(a * 8), 8, px.begin() + static_cast<std::ptrdiff_t>(i * 8));
            cell[i] = cells_[a];
        }
    for (int y = 0; y < to.rows; ++y)
        for (int b = 0; b < to.bytes; ++b) {
            const std::size_t a = (to.at + static_cast<std::uint32_t>(y * to.stride + b)) & 0xFFFF;
            const std::size_t i = static_cast<std::size_t>(y) * from.bytes + b;
            std::copy_n(px.begin() + static_cast<std::ptrdiff_t>(i * 8), 8, shadow_.begin() + static_cast<std::ptrdiff_t>(a * 8));
            cells_[a] = cell[i];
        }
}

void DrawTracker::xor_colour(const Rect& r, std::uint8_t value) {
    if (r.at >= kVram || r.bytes <= 0 || r.rows <= 0) return;
    for (int y = 0; y < r.rows; ++y)
        for (int b = 0; b < r.bytes; ++b) {
            const std::size_t a = (r.at + static_cast<std::uint32_t>(y * r.stride + b)) & 0xFFFF;
            for (std::size_t k = 0; k < 8; ++k) {
                std::uint8_t& p = shadow_[a * 8 + k];
                if (p != kNone && p != kSprite) p = static_cast<std::uint8_t>((p ^ value) & 15);
            }
        }
}

// One character: `count` rows of mask bytes (MSB = left) written in `colour`, `stride` bytes apart.
void DrawTracker::glyph(std::uint32_t at, int stride, const std::uint8_t* rows, int count, std::uint8_t colour) {
    if (at >= kVram) return;
    pages_[0] = 1;
    for (int y = 0; y < count; ++y) {
        const std::size_t a = (at + static_cast<std::uint32_t>(y * stride)) & 0xFFFF;
        cells_[a] = static_cast<std::uint8_t>((y + 1) | (count - 1) << 4);
        for (int b = 0; b < 8; ++b)
            if ((rows[y] >> (7 - b)) & 1) shadow_[a * 8 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(colour & 15);
    }
}

// 3009:88EB: the 8x10 font (DS 124Ah:F3DE, char - 20h, 10 bytes each) into page cs:8DAB.
void DrawTracker::text_88eb(Cpu& cpu) {
    const std::uint8_t* ram = machine_.memory().ram();
    const auto& r = cpu.regs;
    const std::uint16_t page = static_cast<std::uint16_t>(ram[kGameCs + 0x8DAB] | ram[kGameCs + 0x8DAC] << 8);
    std::uint32_t at = vram(page, r.r[DI]);
    std::uint16_t si = r.r[SI];
    std::string text;
    for (int n = 0; n < 256; ++n, ++si) {
        const std::uint8_t c = ram[Cpu::linear(r.s[DS], si)];
        if (c == 0) break;
        text += static_cast<char>(c);
        std::uint8_t rows[10] = {};
        if (c != 0x20) {  // the game skips spaces; their cells still count as the text's
            const std::uint16_t g = static_cast<std::uint16_t>(static_cast<std::uint8_t>(c - 0x20) * 10 + 0xF3DE);
            for (int k = 0; k < 10; ++k) rows[k] = ram[Cpu::linear(kFontSeg8x10, static_cast<std::uint16_t>(g + k))];
        }
        glyph(at, 0x50, rows, 10, r.hi(AX));
        at = at < kVram ? (at + 1) & 0xFFFF : at;
    }
    if (log_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "text88EB page %04X di %04X colour %d: ", page, r.r[DI], r.hi(AX));
        log(buf + text);
    }
}

// 3009:F3E1 (menus): count-prefixed string, the row from cs:8F4A[BX] (halved in 320 mode) plus CX
// bytes, into the page on display (cs:8DA9); even rows masked with cs:E5FE, odd with cs:E5FF.
void DrawTracker::text_f3e1(Cpu& cpu) {
    const std::uint8_t* ram = machine_.memory().ram();
    const auto& r = cpu.regs;
    const auto cs16 = [ram](std::uint32_t off) { return static_cast<std::uint16_t>(ram[kGameCs + off] | ram[kGameCs + off + 1] << 8); };
    std::uint16_t si = r.r[SI];
    const auto count = static_cast<std::int8_t>(ram[Cpu::linear(r.s[DS], si)]);
    if (count <= 0) return;
    const int shift = ram[kGameCs + 0x926A] & 7;
    const std::uint16_t di = static_cast<std::uint16_t>((cs16(0x8F4A + 2u * r.r[BX]) >> shift) + r.r[CX]);
    const std::uint16_t page = cs16(0x8DA9);
    const int stride = ram[Cpu::linear(r.s[DS], 0x7763)];
    const std::uint8_t even = ram[kGameCs + 0xE5FE], odd = ram[kGameCs + 0xE5FF];
    std::uint32_t at = vram(page, di);
    ++si;
    for (int n = 0; n < count; ++n, ++si) {
        const std::uint8_t c = ram[Cpu::linear(r.s[DS], si)];
        if (c >= 0x20) {
            std::uint8_t rows[10];
            const std::uint16_t g = static_cast<std::uint16_t>((c - 0x20) * 10 + 0xF3DE);
            for (int k = 0; k < 10; ++k)
                rows[k] = static_cast<std::uint8_t>(ram[Cpu::linear(kFontSeg8x10, static_cast<std::uint16_t>(g + k))] & (k % 2 ? odd : even));
            glyph(at, stride, rows, 10, r.hi(AX));
        }
        at = at < kVram ? (at + 1) & 0xFFFF : at;
    }
    log("textF3E1");
}

// 3009:5D55: the 8x7 font (segment 3243h at run time, offset 14C0: digits and capitals from '.'),
// into page DX, 40-byte rows.
void DrawTracker::text_5d55(Cpu& cpu) {
    const std::uint8_t* ram = machine_.memory().ram();
    const auto& r = cpu.regs;
    std::uint32_t at = vram(r.r[DX], r.r[DI]);
    std::uint16_t si = r.r[SI];
    std::string text;
    for (int n = 0; n < 256; ++n, ++si) {
        std::uint8_t c = ram[Cpu::linear(r.s[DS], si)];
        if (c == 0) break;
        text += static_cast<char>(c);
        std::uint8_t rows[7] = {};
        if (c != 0x20) {
            if (c >= 0x3A) c &= 0xDF;
            const auto ax = static_cast<std::uint16_t>(c - 0x2E);
            const auto g = static_cast<std::uint16_t>(ax * 7 + 0x14C0);
            for (int k = 0; k < 7; ++k) rows[k] = ram[Cpu::linear(kFontSeg8x7, static_cast<std::uint16_t>(g + k))];
        }
        glyph(at, 0x28, rows, 7, r.hi(AX));
        at = at < kVram ? (at + 1) & 0xFFFF : at;
    }
    if (log_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "text5D55 page %04X di %04X colour %d: ", r.r[DX], r.r[DI], r.hi(AX));
        log(buf + text);
    }
}

// 3009:858F called from 3009:854C: the sprite's mask plane ANDed into page cs:8DAB. The pixels it
// clears are the sprite's (its colour planes are ORed in next); the clip and edge masks are the ones
// the routine computes from cs:7FA1-7FBB.
void DrawTracker::sprite_mask(Cpu& cpu) {
    const std::uint8_t* ram = machine_.memory().ram();
    const auto& r = cpu.regs;
    const std::uint32_t sp = Cpu::linear(r.s[SS], r.r[SP]);
    if ((ram[sp] | ram[sp + 1] << 8) != 0x854F) return;  // a colour plane pass
    const auto cs16 = [ram](std::uint32_t off) { return static_cast<std::uint16_t>(ram[kGameCs + off] | ram[kGameCs + off + 1] << 8); };
    const std::uint16_t page = cs16(0x8DAB);
    std::uint16_t di = cs16(0x7FB7);
    std::uint16_t si = static_cast<std::uint16_t>(r.r[SI] + cs16(0x7FB5));
    const int left = cs16(0x7FA1) & 7;
    std::uint8_t first = static_cast<std::uint8_t>(0xFF >> left), last = 0xFF;
    const int dx = static_cast<std::int16_t>(cs16(0x7FA5) - cs16(0x7FAB));
    if (dx <= 0 || (dx >> 3) == 0) last = static_cast<std::uint8_t>(0xFF << ((cs16(0x7FA5) & 7) ^ 7));
    const int rows = static_cast<std::int16_t>(cs16(0x7FBB));
    const int bytes = static_cast<std::int16_t>(cs16(0x7FB9));
    const std::uint16_t dst_skip = cs16(0x7FB3), src_skip = cs16(0x7FB1);
    if (vram(page, di) >= kVram) return;
    pages_[0] = 1;
    for (int y = 0; y < rows && y < 480; ++y) {
        for (int b = 0; b < std::max(bytes, 1) && b < 128; ++b) {
            const std::uint8_t mask = b == 0 ? first : (b == bytes - 1 ? last : 0xFF);
            const std::uint8_t src = ram[Cpu::linear(r.s[DS], si)];
            const std::uint32_t a = vram(page, di);
            if (a < kVram) {
                const auto written = static_cast<std::uint8_t>(mask & ~src);
                for (int k = 0; k < 8; ++k)
                    if ((written >> (7 - k)) & 1) shadow_[a * 8 + static_cast<std::size_t>(k)] = kSprite;
            }
            ++di;
            ++si;
            if (bytes < 2) break;
        }
        di = static_cast<std::uint16_t>(di + dst_skip);
        si = static_cast<std::uint16_t>(si + src_skip);
    }
}

bool DrawTracker::overlay(const std::uint8_t* frame, int width, int height, std::vector<std::uint8_t>& pixels,
                          std::vector<std::uint8_t>& cells) const {
    if (!pages_[0] || !frame || (width != 640 && width != 320) || height <= 0) return false;
    const std::uint32_t start = machine_.ega().display_start();
    const int bytes_per_row = width / 8;
    const auto n = static_cast<std::size_t>(width) * height;
    pixels.assign(n, kNone);
    cells.assign(n, 0);
    bool any = false;
    // During the screen transition, what it hasn't brought in yet is the old screen: all of it the
    // game's (kept as the frame shows it). The dissolve brings in exact blocks; the other ways count
    // as nothing brought in until they end.
    if (transition_ < kVram && start == transition_)
        for (int y = 0; y < height; ++y)
            for (int xb = 0; xb < bytes_per_row; ++xb) {
                const std::size_t i = static_cast<std::size_t>(y * bytes_per_row + xb);
                if (i < revealed_.size() && revealed_[i]) continue;
                std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(i * 8), 8, kSprite);
                any = true;
            }
    for (int y = 0; y < height; ++y)
        for (int xb = 0; xb < bytes_per_row; ++xb) {
            const std::size_t a = (start + static_cast<std::uint32_t>(y * bytes_per_row + xb)) & 0xFFFF;
            const std::uint8_t* px = &shadow_[a * 8];
            if (!cells_[a] && px[0] == kNone && px[1] == kNone && px[2] == kNone && px[3] == kNone && px[4] == kNone &&
                px[5] == kNone && px[6] == kNone && px[7] == kNone)
                continue;
            const std::size_t row = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(xb) * 8;
            for (std::size_t k = 0; k < 8; ++k) {
                cells[row + k] = cells_[a];
                const std::uint8_t sv = px[k];
                if (pixels[row + k] == kSprite) continue;  // the transition's
                if (sv == kNone || (sv != kSprite && frame[row + k] != sv)) continue;
                pixels[row + k] = sv;
                any = true;
            }
        }
    return any || std::any_of(cells.begin(), cells.end(), [](std::uint8_t c) { return c != 0; });
}

}  // namespace vette::graphics
