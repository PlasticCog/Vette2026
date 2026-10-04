// IBM EGA with 256 KB, after the IBM EGA Technical Reference. See ega.h for the interface.
//
// Video memory is one 32-bit word per CPU offset, byte p = plane p, so the graphics controller's data
// path (rotate, set/reset, logical function, bit mask, map mask) runs on all four planes at once and
// vram_read/vram_write are a few 32-bit operations on values precomputed at register-write time.
//
// Register reads follow the EGA, where most registers are write-only: only input status 0/1 (3C2h,
// 3DAh) and CRTC 0Ch-0Fh (start address, cursor) and 10h/11h (light pen) can be read. Everything else
// reads as an undriven bus (FFh).

#include "host/ega.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

namespace vette::host {

namespace {

constexpr uint32_t kPlaneMask = 0xFFFF;  // 64 KB per plane; the CRTC and CPU offsets wrap here

// Nibble -> FFh in byte p for each set bit p (a per-plane enable mask).
constexpr std::array<uint32_t, 16> make_expand4() {
    std::array<uint32_t, 16> t{};
    for (uint32_t n = 0; n < 16; ++n) {
        for (uint32_t p = 0; p < 4; ++p) {
            if (n & (1u << p)) {
                t[n] |= 0xFFu << (8 * p);
            }
        }
    }
    return t;
}
constexpr std::array<uint32_t, 16> kExpand4 = make_expand4();

constexpr uint32_t splat(uint32_t byte) { return byte * 0x01010101u; }

// Plane byte -> bit 0 of byte i holds pixel i (bit 7-i: the leftmost pixel is the MSB).
constexpr std::array<uint64_t, 256> make_spread() {
    std::array<uint64_t, 256> t{};
    for (uint32_t b = 0; b < 256; ++b) {
        for (uint32_t i = 0; i < 8; ++i) {
            if (b & (0x80u >> i)) {
                t[b] |= uint64_t{1} << (8 * i);
            }
        }
    }
    return t;
}
constexpr std::array<uint64_t, 256> kSpread = make_spread();

// 200-line modes: the IBM 5154 runs CGA-compatible (positive vertical sync) and decodes four bits of
// each palette value, R G B on bits 2 1 0 and intensity on bit 4 (the secondary-green pin), with the
// CGA monitor's brown: red+green without intensity has its green halved.
constexpr uint32_t rgb_200_line(uint8_t v) {
    const uint32_t i = (v & 0x10) ? 0x55 : 0x00;
    const uint32_t r = ((v & 0x04) ? 0xAA : 0x00) + i;
    uint32_t g = ((v & 0x02) ? 0xAA : 0x00) + i;
    const uint32_t b = ((v & 0x01) ? 0xAA : 0x00) + i;
    if ((v & 0x17) == 0x06) {
        g = 0x55;
    }
    return r << 16 | g << 8 | b;
}

// 350-line mode (negative vertical sync): all six bits, rgbRGB, secondary = 55h, primary = AAh.
constexpr uint32_t rgb_350_line(uint8_t v) {
    const uint32_t r = ((v & 0x04) ? 0xAA : 0x00) + ((v & 0x20) ? 0x55 : 0x00);
    const uint32_t g = ((v & 0x02) ? 0xAA : 0x00) + ((v & 0x10) ? 0x55 : 0x00);
    const uint32_t b = ((v & 0x01) ? 0xAA : 0x00) + ((v & 0x08) ? 0x55 : 0x00);
    return r << 16 | g << 8 | b;
}

// EGA BIOS parameter tables (256 KB card, Enhanced Color Display) for the graphics modes we render.
struct ModeParams {
    uint8_t mode;
    int width;
    int height;
    std::array<uint8_t, 4> seq;  // sequencer 01h-04h; 00h (reset) ends at 03h
    uint8_t misc;
    std::array<uint8_t, 25> crtc;
    std::array<uint8_t, 20> attr;
    std::array<uint8_t, 9> gc;
};

constexpr std::array<uint8_t, 20> kAttr200 = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                              0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                              0x01, 0x00, 0x0F, 0x00};
constexpr std::array<uint8_t, 20> kAttr350 = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
                                              0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
                                              0x01, 0x00, 0x0F, 0x00};
constexpr std::array<uint8_t, 9> kGcPlanar = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF};

// Timing: 0Dh and 0Eh use the 14.318 MHz clock (0Dh halves it) with 912 clocks per line and 262 lines,
// so 15.70 kHz and 59.92 Hz; vertical retrace is three lines from line 224 (225 in 0Dh). 10h uses
// 16.257 MHz, 744 clocks per line, 366 lines: 21.85 kHz and 59.70 Hz.
// CRTC 17h is E3h (byte mode) in all three: VETTE's second 0Dh page is at A000:2000, a byte address.
constexpr std::array<ModeParams, 3> kModes = {{
    {0x0D, 320, 200, {0x0B, 0x0F, 0x00, 0x06}, 0x23,
     {0x37, 0x27, 0x2D, 0x37, 0x31, 0x15, 0x04, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xE1, 0x24, 0xC7, 0x14, 0x00, 0xE0, 0xF0, 0xE3, 0xFF},
     kAttr200, kGcPlanar},
    {0x0E, 640, 200, {0x01, 0x0F, 0x00, 0x06}, 0x23,
     {0x70, 0x4F, 0x59, 0x2D, 0x5E, 0x06, 0x04, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xE0, 0x23, 0xC7, 0x28, 0x00, 0xDF, 0xEF, 0xE3, 0xFF},
     kAttr200, kGcPlanar},
    {0x10, 640, 350, {0x01, 0x0F, 0x00, 0x06}, 0xA7,
     {0x5B, 0x4F, 0x53, 0x37, 0x52, 0x00, 0x6C, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x5E, 0x2B, 0x5D, 0x28, 0x0F, 0x5F, 0x0A, 0xE3, 0xFF},
     kAttr350, kGcPlanar},
}};

const ModeParams* find_mode(uint8_t mode) {
    for (const ModeParams& m : kModes) {
        if (m.mode == mode) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

struct Ega::State {
    // --- Graphics controller data path, derived from the registers (hot) -------------------------
    uint32_t latch = 0;
    uint32_t map_mask = 0;       // FFh in each plane byte enabled by the sequencer map mask
    uint32_t set_reset = 0;      // FFh where both set/reset and enable set/reset are 1
    uint32_t sr_enable = 0;      // FFh where enable set/reset is 1
    uint32_t bit_mask = 0;       // bit mask register in every plane byte
    uint32_t color_compare = 0;  // FFh where the color compare bit is 1
    uint32_t color_care = 0;     // FFh where color don't care is 1 (the plane takes part)
    uint8_t write_mode = 0;
    uint8_t rotate = 0;
    uint8_t function = 0;        // 0 replace, 1 AND, 2 OR, 3 XOR (with the latches)
    uint8_t read_shift = 0;      // 8 * read map select
    bool read_compare = false;   // read mode 1
    bool mapped = true;          // memory map select includes A0000h-AFFFFh

    uint8_t mode = 0x03;

    // --- Registers ---------------------------------------------------------------------------------
    uint8_t misc = 0;  // miscellaneous output (3C2h)
    uint8_t feature = 0;
    uint8_t seq_index = 0;
    std::array<uint8_t, 5> seq{};
    uint8_t gc_index = 0;
    std::array<uint8_t, 9> gc{};
    uint8_t crtc_index = 0;
    std::array<uint8_t, 25> crtc{};
    uint8_t attr_index = 0;
    bool attr_data = false;  // attribute flip-flop: the next 3C0h write is data (else an index)
    bool attr_pas = true;    // palette address source: 1 = palette drives the display
    std::array<uint8_t, 20> attr{};

    // --- Display timing, derived from the clock select, clocking mode and CRTC -----------------------
    // Time is counted in ticks of the selected oscillator: ticks = ns * hz_num / ns_den.
    uint64_t hz_num = 0;
    uint64_t ns_den = 1;
    uint32_t char_ticks = 8;
    uint32_t line_ticks = 912;
    uint64_t frame_ticks = 912 * 262;
    uint32_t hdisp = 80;     // characters per line with display enable
    uint32_t vtotal = 262;
    uint32_t vdisp = 200;
    uint32_t vrs = 224;      // first line of vertical retrace
    uint32_t vr_lines = 3;

    // The CRTC loads its start address at the start of vertical retrace: `shown_start` is the value
    // loaded at the last retrace up to `start_checked_ns`.
    uint16_t shown_start = 0;
    uint64_t start_checked_ns = 0;
    // Vertical retrace interrupt flip-flop (input status 0 bit 7), armed when CRTC 11h bit 4 goes 1.
    uint64_t vint_armed_ns = 0;

    std::function<uint64_t()> clock;
    bool clocked = false;

    std::array<uint32_t, kPlaneMask + 1> vram{};  // byte p of each word = plane p

    uint64_t now() const { return clocked ? clock() : 0; }

    uint64_t ticks(uint64_t ns) const { return ns / ns_den * hz_num + ns % ns_den * hz_num / ns_den; }

    void update_timing() {
        if (((misc >> 2) & 0x03) == 1) {
            hz_num = 16'257'000;  // 16.257 MHz
            ns_den = 1'000'000'000;
        } else {
            hz_num = 157'500'000;  // 14.31818 MHz = 157.5 MHz / 11 (the external clock is not modelled)
            ns_den = 11'000'000'000;
        }
        char_ticks = ((seq[1] & 0x01) ? 8u : 9u) * ((seq[1] & 0x08) ? 2u : 1u);
        hdisp = crtc[0x01] + 1u;
        line_ticks = (crtc[0x00] + 2u) * char_ticks;
        vtotal = (crtc[0x06] | ((crtc[0x07] & 0x01u) << 8)) + 2u;
        vdisp = (crtc[0x12] | ((crtc[0x07] & 0x02u) << 7)) + 1u;
        vrs = crtc[0x10] | ((crtc[0x07] & 0x04u) << 6);
        // Retrace ends when the line counter's low four bits equal CRTC 11h bits 0-3.
        vr_lines = (crtc[0x11] - vrs) & 0x0Fu;
        if (vr_lines == 0) {
            vr_lines = 16;
        }
        frame_ticks = uint64_t{line_ticks} * vtotal;
    }

    bool in_vretrace(uint32_t line) const {
        if (vrs >= vtotal) {
            return false;
        }
        const uint32_t since = line >= vrs ? line - vrs : line + vtotal - vrs;
        return since < vr_lines;
    }

    // Whether a vertical retrace started in (from_ns, to_ns]. Retraces start at k * frame + vrs * line
    // ticks, so this compares frame numbers after shifting by that offset (plus a frame, to stay >= 0).
    bool retrace_began(uint64_t from_ns, uint64_t to_ns) const {
        if (to_ns <= from_ns || vrs >= vtotal) {
            return false;
        }
        const uint64_t shift = frame_ticks - uint64_t{vrs} * line_ticks;
        return (ticks(to_ns) + shift) / frame_ticks != (ticks(from_ns) + shift) / frame_ticks;
    }

    // Emulated time of the next vertical retrace start (or end) strictly after `ns`.
    uint64_t next_vretrace_edge(uint64_t ns, bool start) const {
        if (vrs >= vtotal) {
            return std::numeric_limits<uint64_t>::max();
        }
        const uint64_t edge = (uint64_t{vrs} + (start ? 0u : vr_lines)) * line_ticks % frame_ticks;
        const uint64_t now_ticks = ticks(ns);
        const uint64_t pos = now_ticks % frame_ticks;
        const uint64_t ahead = pos < edge ? edge - pos : frame_ticks - pos + edge;
        // The first ns whose tick count reaches the edge: ceil(target * ns_den / hz_num), split so it
        // can't overflow.
        const uint64_t target = now_ticks + ahead;
        const uint64_t whole = target / hz_num * ns_den;
        const uint64_t part = (target % hz_num * ns_den + hz_num - 1) / hz_num;
        return std::max(whole + part, ns + 1);
    }

    uint8_t input_status0() const {
        // Bit 4: the configuration switch picked by the clock select bits (CS=0 reads switch 4 ...
        // CS=3 switch 1), for switches 1001b: an Enhanced Color Display, the usual setting.
        constexpr uint32_t kSwitches = 0x09;
        const uint32_t cs = (misc >> 2) & 0x03u;
        uint8_t v = ((kSwitches >> (3 - cs)) & 1) ? 0x10 : 0x00;
        if ((crtc[0x11] & 0x10) && retrace_began(vint_armed_ns, now())) {
            v |= 0x80;
        }
        return v;
    }

    uint8_t input_status1() const {
        const uint64_t pos = ticks(now()) % frame_ticks;
        const auto line = static_cast<uint32_t>(pos / line_ticks);
        const auto column = static_cast<uint32_t>(pos % line_ticks / char_ticks);
        uint8_t v = 0x04;  // light pen switch open (no light pen); strobe (bit 1) clear
        if (line >= vdisp || column >= hdisp) {
            v |= 0x01;  // display enable inactive: horizontal or vertical non-display interval
        }
        if (in_vretrace(line)) {
            v |= 0x08;
        }
        return v;
    }

    uint16_t start_register() const { return static_cast<uint16_t>(crtc[0x0C] << 8 | crtc[0x0D]); }

    uint16_t displayed_start() const {
        if (!clocked) {
            return start_register();
        }
        return retrace_began(start_checked_ns, now()) ? start_register() : shown_start;
    }

    // Before the start address changes: bring the latched copy up to date.
    void sync_start_latch() {
        const uint64_t t = now();
        if (retrace_began(start_checked_ns, t)) {
            shown_start = start_register();
        }
        start_checked_ns = t;
    }

    void reset_start_latch() {
        shown_start = start_register();
        start_checked_ns = now();
    }

    void update_gc(uint8_t index) {
        const uint8_t v = gc[index];
        switch (index) {
        case 0x00:
        case 0x01:
            set_reset = kExpand4[gc[0] & gc[1] & 0x0F];
            sr_enable = kExpand4[gc[1] & 0x0F];
            break;
        case 0x02: color_compare = kExpand4[v & 0x0F]; break;
        case 0x03:
            rotate = v & 0x07;
            function = (v >> 3) & 0x03;
            break;
        case 0x04: read_shift = static_cast<uint8_t>(8 * (v & 0x03)); break;
        case 0x05:
            // Write mode 3 is reserved on the EGA; treat it as mode 0.
            write_mode = (v & 0x03) == 3 ? 0 : v & 0x03;
            read_compare = (v & 0x08) != 0;
            break;
        case 0x06: mapped = ((v >> 2) & 0x03) < 2; break;  // A0000h-BFFFFh or A0000h-AFFFFh
        case 0x07: color_care = kExpand4[v & 0x0F]; break;
        case 0x08: bit_mask = splat(v); break;
        default: break;
        }
    }

    void refresh_all() {
        map_mask = kExpand4[seq[2] & 0x0F];
        for (uint8_t i = 0; i < gc.size(); ++i) {
            update_gc(i);
        }
        update_timing();
    }

    void write_seq(uint8_t v) {
        if (seq_index >= seq.size()) {
            return;
        }
        seq[seq_index] = v;
        if (seq_index == 0x02) {
            map_mask = kExpand4[v & 0x0F];
        } else if (seq_index == 0x01) {
            update_timing();
        }
    }

    void write_gc(uint8_t v) {
        if (gc_index >= gc.size()) {
            return;
        }
        gc[gc_index] = v;
        update_gc(gc_index);
    }

    void write_crtc(uint8_t v) {
        if (crtc_index >= crtc.size()) {
            return;
        }
        if (crtc_index == 0x0C || crtc_index == 0x0D) {
            sync_start_latch();
        } else if (crtc_index == 0x11 && !(crtc[0x11] & 0x10) && (v & 0x10)) {
            vint_armed_ns = now();
        }
        crtc[crtc_index] = v;
        switch (crtc_index) {
        case 0x00:
        case 0x01:
        case 0x06:
        case 0x07:
        case 0x10:
        case 0x11:
        case 0x12: update_timing(); break;
        default: break;
        }
    }

    uint8_t read_crtc() const {
        if (crtc_index >= 0x0C && crtc_index <= 0x0F) {
            return crtc[crtc_index];
        }
        if (crtc_index == 0x10 || crtc_index == 0x11) {
            return 0x00;  // light pen address
        }
        return 0xFF;
    }

    void write_attr_register(uint8_t index, uint8_t v) {
        if (index < 0x10 || index == 0x11) {
            attr[index] = v & 0x3F;  // six-bit color values
        } else if (index < attr.size()) {
            attr[index] = v;
        }
    }

    void write_attr_port(uint8_t v) {
        if (!attr_data) {
            attr_index = v & 0x1F;
            attr_pas = (v & 0x20) != 0;
        } else if (attr_index >= 0x10 || !attr_pas) {
            // While the palette drives the display (PAS = 1) the CPU can't load it.
            write_attr_register(attr_index, v);
        }
        attr_data = !attr_data;
    }
};

Ega::Ega() : s_(std::make_unique<State>()) {
    // Power-on: the registers hold the mode 0Eh set (a working planar device with valid timing) and
    // the reported mode is 03h, as the BIOS leaves it after POST.
    set_mode(0x0E);
    s_->mode = 0x03;
}

Ega::~Ega() = default;

uint8_t Ega::vram_read(uint32_t offset) {
    State& s = *s_;
    if (!s.mapped) {
        return 0xFF;
    }
    const uint32_t planes = s.vram[offset & kPlaneMask];
    s.latch = planes;
    if (!s.read_compare) {
        return static_cast<uint8_t>(planes >> s.read_shift);
    }
    // Read mode 1: a bit is 1 where every plane that takes part matches its color compare bit.
    const uint32_t diff = (planes ^ s.color_compare) & s.color_care;
    return static_cast<uint8_t>(~(diff | diff >> 8 | diff >> 16 | diff >> 24));
}

void Ega::vram_write(uint32_t offset, uint8_t value) {
    State& s = *s_;
    if (!s.mapped) {
        return;
    }
    uint32_t& cell = s.vram[offset & kPlaneMask];
    uint32_t data;
    if (s.write_mode == 1) {
        data = s.latch;
    } else {
        if (s.write_mode == 2) {
            data = kExpand4[value & 0x0F];
        } else {
            const uint32_t v = value;
            const uint32_t rotated = (v >> s.rotate | v << (8 - s.rotate)) & 0xFF;
            data = (splat(rotated) & ~s.sr_enable) | s.set_reset;
        }
        switch (s.function) {
        case 1: data &= s.latch; break;
        case 2: data |= s.latch; break;
        case 3: data ^= s.latch; break;
        default: break;
        }
        data = (data & s.bit_mask) | (s.latch & ~s.bit_mask);
    }
    cell = (cell & ~s.map_mask) | (data & s.map_mask);
}

bool Ega::handles(uint16_t port) {
    return (port >= 0x3C0 && port <= 0x3CF) || port == 0x3D4 || port == 0x3D5 || port == 0x3DA;
}

uint8_t Ega::in8(uint16_t port) {
    State& s = *s_;
    switch (port) {
    case 0x3C2: return s.input_status0();
    case 0x3D5: return s.read_crtc();
    case 0x3DA:
        s.attr_data = false;
        return s.input_status1();
    default: return 0xFF;
    }
}

void Ega::out8(uint16_t port, uint8_t value) {
    State& s = *s_;
    switch (port) {
    case 0x3C0:
    case 0x3C1: s.write_attr_port(value); break;  // 3C1h also reaches the attribute controller
    case 0x3C2:
        s.misc = value;
        s.update_timing();
        break;
    case 0x3C4: s.seq_index = value & 0x07; break;
    case 0x3C5: s.write_seq(value); break;
    case 0x3CE: s.gc_index = value & 0x0F; break;
    case 0x3CF: s.write_gc(value); break;
    case 0x3D4: s.crtc_index = value & 0x1F; break;
    case 0x3D5: s.write_crtc(value); break;
    case 0x3DA: s.feature = value; break;  // feature control
    default: break;                        // 3CAh/3CCh graphics position registers, unused ports
    }
}

void Ega::set_mode(uint8_t mode, bool clear) {
    State& s = *s_;
    if (mode & 0x80) {  // INT 10h AH=00h: bit 7 = keep video memory
        clear = false;
        mode &= 0x7F;
    }
    s.mode = mode;
    const ModeParams* p = find_mode(mode);
    if (!p) {
        return;  // text and other modes are only recorded
    }
    s.misc = p->misc;
    s.seq[0] = 0x03;
    std::copy(p->seq.begin(), p->seq.end(), s.seq.begin() + 1);
    s.crtc = p->crtc;
    s.attr = p->attr;
    s.attr_index = 0x00;
    s.attr_pas = true;  // the BIOS ends by writing 20h: palette enabled
    s.attr_data = false;
    s.gc = p->gc;
    s.refresh_all();
    s.reset_start_latch();
    if (clear) {
        s.vram.fill(0);
    }
}

uint8_t Ega::mode() const { return s_->mode; }

void Ega::set_palette_register(uint8_t index, uint8_t ega_color) {
    // The BIOS resets the flip-flop, writes the register with the palette address source off and
    // then writes 20h, so the palette drives the display again. Any attribute register (0-13h) works.
    State& s = *s_;
    s.write_attr_register(index, ega_color);
    s.attr_index = 0x00;
    s.attr_pas = true;
    s.attr_data = false;
}

void Ega::set_border(uint8_t ega_color) { set_palette_register(0x11, ega_color); }

void Ega::set_time_source(std::function<uint64_t()> now_ns) {
    State& s = *s_;
    s.clocked = static_cast<bool>(now_ns);
    s.clock = std::move(now_ns);
    s.reset_start_latch();
    s.vint_armed_ns = s.now();
}

uint16_t Ega::display_start() const { return static_cast<uint16_t>(s_->displayed_start()); }

bool Ega::in_vertical_retrace() const { return (s_->input_status1() & 0x08) != 0; }

uint64_t Ega::next_vertical_retrace_ns(bool start) const { return s_->next_vretrace_edge(s_->now(), start); }

void Ega::render(Frame& out) const { render_page(display_start(), out); }

void Ega::render_page(uint16_t start_address, Frame& out) const {
    const State& s = *s_;
    const bool lines350 = (s.misc & 0x80) != 0;  // negative vertical sync: 350-line monitor mode
    for (size_t i = 0; i < out.palette.size(); ++i) {
        out.palette[i] = lines350 ? rgb_350_line(s.attr[i]) : rgb_200_line(s.attr[i]);
    }
    const ModeParams* p = find_mode(s.mode);
    if (!p) {
        out.width = 0;
        out.height = 0;
        out.pixels.clear();
        return;
    }
    out.width = p->width;
    out.height = p->height;
    out.pixels.assign(static_cast<size_t>(p->width) * static_cast<size_t>(p->height), 0);

    if (!s.attr_pas) {
        // The palette is disconnected from the video path: the whole screen shows the overscan color.
        out.palette.fill(lines350 ? rgb_350_line(s.attr[0x11]) : rgb_200_line(s.attr[0x11]));
        return;
    }

    const uint32_t start = start_address;
    const uint32_t stride = s.crtc[0x13] * 2u;  // byte mode: the offset register counts words
    const uint64_t enable = (s.attr[0x12] & 0x0Fu) * 0x0101010101010101ull;  // color plane enable
    const int bytes_per_row = p->width / 8;
    uint8_t* dst = out.pixels.data();
    for (int y = 0; y < p->height; ++y) {
        const uint32_t row = start + static_cast<uint32_t>(y) * stride;
        for (int x = 0; x < bytes_per_row; ++x) {
            const uint32_t planes = s.vram[(row + static_cast<uint32_t>(x)) & kPlaneMask];
            const uint64_t eight = (kSpread[planes & 0xFF] | kSpread[(planes >> 8) & 0xFF] << 1 |
                                    kSpread[(planes >> 16) & 0xFF] << 2 | kSpread[planes >> 24] << 3) &
                                   enable;
            for (int i = 0; i < 8; ++i) {
                *dst++ = static_cast<uint8_t>(eight >> (8 * i));
            }
        }
    }
}

void Ega::copy_state_from(const Ega& other) {
    std::function<uint64_t()> clock = std::move(s_->clock);
    const bool clocked = s_->clocked;
    *s_ = *other.s_;
    s_->clock = std::move(clock);
    s_->clocked = clocked;
}

std::string Ega::diff_state(const Ega& other, size_t max_items) const {
    const State& a = *s_;
    const State& b = *other.s_;
    std::string out;
    size_t items = 0;
    auto note = [&](const char* what, unsigned index, unsigned va, unsigned vb) {
        if (items++ < max_items) {
            char line[96];
            std::snprintf(line, sizeof line, "%s[%X]: %X vs %X\n", what, index, va, vb);
            out += line;
        }
    };
    auto regs = [&](const char* what, const auto& ra, const auto& rb) {
        for (size_t i = 0; i < ra.size(); ++i) {
            if (ra[i] != rb[i]) {
                note(what, static_cast<unsigned>(i), ra[i], rb[i]);
            }
        }
    };
    if (a.latch != b.latch) note("latch", 0, a.latch, b.latch);
    if (a.mode != b.mode) note("mode", 0, a.mode, b.mode);
    if (a.misc != b.misc) note("misc", 0, a.misc, b.misc);
    if (a.seq_index != b.seq_index) note("seq_index", 0, a.seq_index, b.seq_index);
    if (a.gc_index != b.gc_index) note("gc_index", 0, a.gc_index, b.gc_index);
    if (a.crtc_index != b.crtc_index) note("crtc_index", 0, a.crtc_index, b.crtc_index);
    if (a.attr_index != b.attr_index) note("attr_index", 0, a.attr_index, b.attr_index);
    if (a.attr_data != b.attr_data) note("attr_flipflop", 0, a.attr_data, b.attr_data);
    regs("seq", a.seq, b.seq);
    regs("gc", a.gc, b.gc);
    regs("crtc", a.crtc, b.crtc);
    regs("attr", a.attr, b.attr);
    for (uint32_t off = 0; off <= kPlaneMask; ++off) {
        if (a.vram[off] != b.vram[off]) {
            note("vram(planes 3210)", off, a.vram[off], b.vram[off]);
        }
    }
    if (items > max_items) {
        out += "... " + std::to_string(items - max_items) + " more\n";
    }
    return out;
}

} // namespace vette::host
