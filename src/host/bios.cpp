#include "host/bios.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "host/hle.h"

namespace vette::host {
namespace {

constexpr uint32_t kRom = 0xF0000;
constexpr uint32_t kTicksPerDay = 0x1800B0;

// US layout, scan-code set 1 make codes 00h-39h.
constexpr char kNormal[] =
    "\0" "\x1b" "1234567890-=" "\b\t" "qwertyuiop[]" "\r\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
constexpr char kShifted[] =
    "\0" "\x1b" "!@#$%^&*()_+" "\b\0" "QWERTYUIOP{}" "\r\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";
static_assert(sizeof(kNormal) == 0x3A + 1 && sizeof(kShifted) == 0x3A + 1);

uint8_t bcd(int v) { return static_cast<uint8_t>(((v / 10) % 10) << 4 | (v % 10)); }

} // namespace

Bios::Bios(Memory& mem, Ega& ega, Options options) : mem_(mem), ega_(ega), opt_(options) {}

void Bios::install() {
    uint8_t* ram = mem_.ram();
    uint16_t pos = 0x0100;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        const uint16_t at = pos;
        for (uint8_t b : bytes) {
            ram[kRom + pos++] = b;
        }
        return at;
    };
    auto set_vector = [&](int v, uint16_t off) {
        mem_.write16(static_cast<uint32_t>(v * 4), off);
        mem_.write16(static_cast<uint32_t>(v * 4 + 2), 0xF000);
    };

    const uint16_t iret = emit({0xCF});
    const uint16_t eoi_iret = emit({0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF});
    for (int v = 0; v < 256; ++v) {
        set_vector(v, v >= 0x08 && v <= 0x0F ? eoi_iret : iret);
    }
    // IRQ0, as the IBM BIOS: STI, count the tick, INT 1Ch, EOI.
    set_vector(0x08, emit({0xFB, 0x50, 0x0F, 0xFF, kTimerTick, 0xCD, 0x1C, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF}));
    // IRQ1: read the scan code (which frees the controller for the next byte), buffer it, EOI.
    set_vector(0x09, emit({0x50, 0xE4, 0x60, 0x0F, 0xFF, kKeyboardIrq, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF}));
    for (uint8_t id : {kDivideError, kInvalidOpcode, kVideo, kEquipment, kMemorySize, kDisk, kSerial,
                       kSystem, kKeyboard, kPrinter, kTime, kDosTerminate, kDos, kMultiplex, kMouse}) {
        set_vector(id, emit({0x0F, 0xFF, id, 0xCF}));
    }
    set_vector(0x24, emit({0xB0, 0x03, 0xCF}));  // DOS critical error handler: fail
    std::memcpy(ram + 0xFFFF5, "01/10/88", 8);
    ram[0xFFFFE] = 0xFC;  // model byte: PC/AT

    // BIOS data area.
    std::memset(ram + 0x400, 0, 0x100);
    set_bda16(0x10, static_cast<uint16_t>(0x0001 | (opt_.game_port ? 0x1000 : 0)));  // floppy, EGA
    set_bda16(0x13, 640);
    set_bda16(0x1A, 0x1E);
    set_bda16(0x1C, 0x1E);
    set_bda16(0x80, 0x1E);
    set_bda16(0x82, 0x3E);
    set_bda8(0x49, 0x03);
    set_bda16(0x4A, 80);
    set_bda16(0x4C, 0x1000);
    set_bda16(0x63, 0x3D4);
    set_bda8(0x84, 24);
    set_bda16(0x85, 8);
    set_bda8(0x87, 0x60);  // EGA: 256 KB, active
    set_bda8(0x88, 0x09);  // EGA switches: enhanced color display
    ega_.set_mode(0x03);

    mem_.protect_from(kRom);
}

void Bios::log_once(const std::string& what) {
    if (log_ && logged_.insert(what).second) {
        log_(what);
    }
}

void Bios::handle(Cpu& cpu, uint8_t id) {
    Registers& r = cpu.regs;
    switch (id) {
    case kTimerTick: {
        uint32_t ticks = mem_.read16(0x46C) | (static_cast<uint32_t>(mem_.read16(0x46E)) << 16);
        if (++ticks >= kTicksPerDay) {
            ticks = 0;
            set_bda8(0x70, 1);
        }
        set_bda16(0x6C, static_cast<uint16_t>(ticks));
        set_bda16(0x6E, static_cast<uint16_t>(ticks >> 16));
        break;
    }
    case kKeyboardIrq:
        keyboard_irq(cpu);
        break;
    case kVideo:
        video(cpu);
        break;
    case kEquipment:
        r.r[AX] = bda16(0x10);
        break;
    case kMemorySize:
        r.r[AX] = bda16(0x13);
        break;
    case kDisk:
        r.set_hi(AX, r.hi(AX) == 0 ? 0 : 1);
        hle::set_carry(cpu, r.hi(AX) != 0);
        break;
    case kSerial:
        r.set_hi(AX, 0x80);  // timeout: no UART modelled yet
        break;
    case kSystem:
        if (r.hi(AX) == 0x88) {
            r.r[AX] = 0;     // no extended memory
            hle::set_carry(cpu, false);
        } else {
            r.set_hi(AX, 0x86);  // unsupported
            hle::set_carry(cpu, true);
        }
        break;
    case kKeyboard:
        keyboard_service(cpu);
        break;
    case kPrinter:
        r.set_hi(AX, 0x08);  // I/O error: no printer
        break;
    case kTime:
        time_service(cpu);
        break;
    case kMultiplex:
        break;  // nothing installed: AL stays 0
    case kMouse:
        mouse_service(cpu);
        break;
    case kDivideError:
    case kInvalidOpcode: {
        const uint16_t ip = mem_.read16(hle::frame_addr(cpu, 0));
        const uint16_t cs = mem_.read16(hle::frame_addr(cpu, 2));
        char msg[96];
        std::snprintf(msg, sizeof msg, "%s at %04X:%04X", id == kDivideError ? "divide error" : "invalid opcode",
                      cs, ip);
        if (fault_) {
            fault_(msg);
        }
        break;
    }
    default:
        log_once("unhandled BIOS callback " + std::to_string(id));
        break;
    }
}

void Bios::video(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint8_t ah = r.hi(AX);
    const uint8_t al = r.lo(AX);
    switch (ah) {
    case 0x00: {
        const uint8_t mode = al & 0x7F;
        ega_.set_mode(mode, (al & 0x80) == 0);
        set_bda8(0x49, mode);
        set_bda16(0x4A, mode == 0x0D || mode <= 0x01 ? 40 : 80);
        set_bda16(0x4C, mode == 0x0D ? 0x2000 : mode >= 0x0E ? 0x4000 : 0x1000);
        set_bda16(0x4E, 0);
        set_bda8(0x62, 0);
        for (uint16_t p = 0; p < 8; ++p) {
            set_bda16(static_cast<uint16_t>(0x50 + p * 2), 0);
        }
        break;
    }
    case 0x01:
        set_bda16(0x60, r.r[CX]);
        break;
    case 0x02:
        set_bda16(static_cast<uint16_t>(0x50 + (r.hi(BX) & 7) * 2), r.r[DX]);
        break;
    case 0x03:
        r.r[DX] = bda16(static_cast<uint16_t>(0x50 + (r.hi(BX) & 7) * 2));
        r.r[CX] = bda16(0x60);
        break;
    case 0x05: {
        const uint16_t start = static_cast<uint16_t>(al * bda16(0x4C));
        set_bda8(0x62, al);
        set_bda16(0x4E, start);
        ega_.out8(0x3D4, 0x0C);
        ega_.out8(0x3D5, static_cast<uint8_t>(start >> 8));
        ega_.out8(0x3D4, 0x0D);
        ega_.out8(0x3D5, static_cast<uint8_t>(start));
        break;
    }
    case 0x0F:
        r.set_lo(AX, bda8(0x49));
        r.set_hi(AX, static_cast<uint8_t>(bda16(0x4A)));
        r.set_hi(BX, bda8(0x62));
        break;
    case 0x10:
        if (al == 0x00) {
            ega_.set_palette_register(r.lo(BX), r.hi(BX));
        } else if (al == 0x01) {
            ega_.set_border(r.hi(BX));
        } else if (al == 0x02) {
            for (uint8_t i = 0; i < 16; ++i) {
                ega_.set_palette_register(i, mem_.read8(Cpu::linear(r.s[ES], static_cast<uint16_t>(r.r[DX] + i))));
            }
            ega_.set_border(mem_.read8(Cpu::linear(r.s[ES], static_cast<uint16_t>(r.r[DX] + 16))));
        }
        break;
    case 0x12:
        if (r.lo(BX) == 0x10) {  // EGA information
            r.r[BX] = 0x0003;    // color, 256 KB
            r.r[CX] = 0x0009;    // no feature bits, enhanced color display switches
        }
        break;
    case 0x1A:
    case 0x1B:
        break;  // VGA-only: leaving AL unchanged tells the caller "no VGA"
    default:
        log_once("unhandled INT 10h AH=" + std::to_string(ah));
        break;
    }
}

void Bios::push_key(uint16_t key) {
    const uint16_t start = bda16(0x80), end = bda16(0x82);
    const uint16_t tail = bda16(0x1C);
    uint16_t next = static_cast<uint16_t>(tail + 2);
    if (next >= end) {
        next = start;
    }
    if (next == bda16(0x1A)) {
        return;  // buffer full (the real BIOS beeps)
    }
    mem_.write16(0x400u + tail, key);
    set_bda16(0x1C, next);
}

int Bios::read_key(bool remove) {
    const uint16_t head = bda16(0x1A);
    if (head == bda16(0x1C)) {
        return -1;
    }
    const uint16_t key = mem_.read16(0x400u + head);
    if (remove) {
        uint16_t next = static_cast<uint16_t>(head + 2);
        if (next >= bda16(0x82)) {
            next = bda16(0x80);
        }
        set_bda16(0x1A, next);
    }
    return key;
}

void Bios::keyboard_irq(Cpu& cpu) {
    const uint8_t sc = cpu.regs.lo(AX);
    if (sc == 0xE0) {
        e0_prefix_ = true;
        return;
    }
    const bool ext = e0_prefix_;
    e0_prefix_ = false;
    const bool release = (sc & 0x80) != 0;
    const uint8_t code = sc & 0x7F;
    uint8_t flags = bda8(0x17);
    auto shift_key = [&](uint8_t bit) {
        flags = static_cast<uint8_t>(release ? (flags & ~bit) : (flags | bit));
        set_bda8(0x17, flags);
    };
    auto toggle_key = [&](uint8_t bit) {
        if (!release) {
            set_bda8(0x17, static_cast<uint8_t>(flags ^ bit));
        }
    };
    switch (code) {
    case 0x2A: if (!ext) shift_key(0x02); return;  // E0 2A is a fake shift from the enhanced keyboard
    case 0x36: if (!ext) shift_key(0x01); return;
    case 0x1D: shift_key(0x04); return;
    case 0x38: shift_key(0x08); return;
    case 0x3A: toggle_key(0x40); return;
    case 0x45: toggle_key(0x20); return;
    case 0x46: toggle_key(0x10); return;
    default: break;
    }
    if (release) {
        return;
    }
    uint8_t ascii = 0;
    const bool shifted = (flags & 0x03) != 0;
    if (ext) {
        ascii = code == 0x1C ? '\r' : code == 0x35 ? '/' : 0;
    } else if (code < 0x3A) {
        ascii = static_cast<uint8_t>(shifted ? kShifted[code] : kNormal[code]);
        if ((flags & 0x40) && ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z'))) {
            ascii ^= 0x20;  // caps lock inverts letter case
        }
        if (flags & 0x04) {
            ascii = (ascii >= '@' && ascii <= 0x7F) ? static_cast<uint8_t>(ascii & 0x1F) : 0;
        }
        if (flags & 0x08) {
            ascii = 0;
        }
    } else if (code == 0x4A) {
        ascii = '-';
    } else if (code == 0x4E) {
        ascii = '+';
    }
    push_key(static_cast<uint16_t>(code << 8 | ascii));
}

void Bios::keyboard_service(Cpu& cpu) {
    Registers& r = cpu.regs;
    switch (r.hi(AX)) {
    case 0x00:
    case 0x10: {
        const int key = read_key(true);
        if (key < 0) {
            hle::retry_int(cpu);  // wait: re-run INT 16h after IRET so IRQs can deliver the key
        } else {
            r.r[AX] = static_cast<uint16_t>(key);
        }
        break;
    }
    case 0x01:
    case 0x11: {
        const int key = read_key(false);
        hle::set_return_flag(cpu, flag::ZF, key < 0);
        if (key >= 0) {
            r.r[AX] = static_cast<uint16_t>(key);
        }
        break;
    }
    case 0x02:
    case 0x12:
        r.set_lo(AX, bda8(0x17));
        break;
    case 0x05:
        push_key(r.r[CX]);
        r.set_lo(AX, 0);
        break;
    default:
        log_once("unhandled INT 16h AH=" + std::to_string(r.hi(AX)));
        break;
    }
}

void Bios::time_service(Cpu& cpu) {
    Registers& r = cpu.regs;
    const RealTime now = clock_ ? clock_() : RealTime{1989, 10, 23, 12, 0, 0, 0};
    switch (r.hi(AX)) {
    case 0x00:
        r.r[CX] = bda16(0x6E);
        r.r[DX] = bda16(0x6C);
        r.set_lo(AX, bda8(0x70));
        set_bda8(0x70, 0);
        break;
    case 0x01:
        set_bda16(0x6E, r.r[CX]);
        set_bda16(0x6C, r.r[DX]);
        set_bda8(0x70, 0);
        break;
    case 0x02:
        r.r[CX] = static_cast<uint16_t>(bcd(now.hour) << 8 | bcd(now.minute));
        r.r[DX] = static_cast<uint16_t>(bcd(now.second) << 8);
        hle::set_carry(cpu, false);
        break;
    case 0x04:
        r.r[CX] = static_cast<uint16_t>(bcd(now.year / 100) << 8 | bcd(now.year % 100));
        r.r[DX] = static_cast<uint16_t>(bcd(now.month) << 8 | bcd(now.day));
        hle::set_carry(cpu, false);
        break;
    default:
        hle::set_carry(cpu, false);  // setting the RTC: accepted and ignored
        break;
    }
}

void Bios::mouse_motion(int dx, int dy) {
    Mouse& m = mouse_;
    m.mickey_x += dx;
    m.mickey_y += dy;
    m.frac_x += dx * 8;
    m.frac_y += dy * 8;
    const int px = m.frac_x / m.ratio_x;
    const int py = m.frac_y / m.ratio_y;
    m.frac_x -= px * m.ratio_x;
    m.frac_y -= py * m.ratio_y;
    m.x = std::clamp(m.x + px, m.x_min, m.x_max);
    m.y = std::clamp(m.y + py, m.y_min, m.y_max);
}

void Bios::mouse_buttons(uint8_t mask) {
    Mouse& m = mouse_;
    for (int b = 0; b < 2; ++b) {
        const bool was = (m.buttons >> b) & 1, now = (mask >> b) & 1;
        if (now && !was) {
            ++m.presses[b];
            m.press_x[b] = static_cast<int16_t>(m.x);
            m.press_y[b] = static_cast<int16_t>(m.y);
        } else if (!now && was) {
            ++m.releases[b];
            m.release_x[b] = static_cast<int16_t>(m.x);
            m.release_y[b] = static_cast<int16_t>(m.y);
        }
    }
    m.buttons = mask & 3;
}

void Bios::mouse_service(Cpu& cpu) {
    Registers& r = cpu.regs;
    Mouse& m = mouse_;
    const uint16_t fn = r.r[AX];
    if (!opt_.mouse_installed) {
        if (fn == 0x00) {
            r.r[AX] = 0;
        }
        return;
    }
    auto clamp_pos = [&] {
        m.x = std::clamp(m.x, m.x_min, m.x_max);
        m.y = std::clamp(m.y, m.y_min, m.y_max);
    };
    switch (fn) {
    case 0x00:
    case 0x21: {
        const uint8_t buttons = m.buttons;
        m = Mouse{};
        m.buttons = buttons;
        r.r[AX] = 0xFFFF;
        r.r[BX] = 2;
        break;
    }
    case 0x01:
        if (m.show_count < 0 && ++m.show_count == 0) {
            log_once("mouse cursor shown (driver cursor is not drawn yet)");
        }
        break;
    case 0x02:
        --m.show_count;
        break;
    case 0x03:
        r.r[BX] = m.buttons;
        r.r[CX] = static_cast<uint16_t>(m.x);
        r.r[DX] = static_cast<uint16_t>(m.y);
        break;
    case 0x04:
        m.x = static_cast<int16_t>(r.r[CX]);
        m.y = static_cast<int16_t>(r.r[DX]);
        clamp_pos();
        break;
    case 0x05:
    case 0x06: {
        const int b = std::min<int>(r.r[BX], 1);
        const bool press = fn == 0x05;
        r.r[AX] = m.buttons;
        r.r[BX] = press ? m.presses[b] : m.releases[b];
        r.r[CX] = static_cast<uint16_t>(press ? m.press_x[b] : m.release_x[b]);
        r.r[DX] = static_cast<uint16_t>(press ? m.press_y[b] : m.release_y[b]);
        (press ? m.presses[b] : m.releases[b]) = 0;
        break;
    }
    case 0x07:
        m.x_min = std::min<int16_t>(static_cast<int16_t>(r.r[CX]), static_cast<int16_t>(r.r[DX]));
        m.x_max = std::max<int16_t>(static_cast<int16_t>(r.r[CX]), static_cast<int16_t>(r.r[DX]));
        clamp_pos();
        break;
    case 0x08:
        m.y_min = std::min<int16_t>(static_cast<int16_t>(r.r[CX]), static_cast<int16_t>(r.r[DX]));
        m.y_max = std::max<int16_t>(static_cast<int16_t>(r.r[CX]), static_cast<int16_t>(r.r[DX]));
        clamp_pos();
        break;
    case 0x0B:
        r.r[CX] = static_cast<uint16_t>(static_cast<int16_t>(m.mickey_x));
        r.r[DX] = static_cast<uint16_t>(static_cast<int16_t>(m.mickey_y));
        m.mickey_x = m.mickey_y = 0;
        break;
    case 0x0F:
        m.ratio_x = std::max<int>(r.r[CX], 1);
        m.ratio_y = std::max<int>(r.r[DX], 1);
        break;
    case 0x1A:
    case 0x1D:
        break;
    case 0x1B:
        r.r[BX] = r.r[CX] = r.r[DX] = 50;
        break;
    case 0x1E:
        r.r[BX] = 0;
        break;
    case 0x24:
        r.r[BX] = 0x0805;
        r.r[CX] = 0x0400;  // PS/2 mouse
        break;
    default:
        log_once("unhandled INT 33h AX=" + std::to_string(fn));
        break;
    }
}

} // namespace vette::host
