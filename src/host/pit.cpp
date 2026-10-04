#include "host/pit.h"

#include <limits>

namespace vette::host {

Pit::Pit() {
    ch_[2].gate = false;  // port 61h bit 0 starts low
}

uint16_t Pit::count(int ch, uint64_t now) const {
    const Channel& c = ch_[static_cast<size_t>(ch)];
    if (!c.gate && (c.mode == 2 || c.mode == 3)) {
        return static_cast<uint16_t>(c.reload);
    }
    const uint64_t elapsed = now - c.start;
    switch (c.mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        return static_cast<uint16_t>(c.reload - elapsed);  // keeps wrapping through 0, as hardware does
    case 3: {
        // Square wave: the count drops by 2 per clock, reloading at each half period.
        const uint64_t phase = elapsed % c.reload;
        const uint64_t half = (c.reload + 1) / 2;
        const uint64_t into = phase < half ? phase : phase - half;
        return static_cast<uint16_t>(c.reload - 2 * into);
    }
    default:  // mode 2 rate generator
        return static_cast<uint16_t>(c.reload - elapsed % c.reload);
    }
}

bool Pit::output(int ch, uint64_t now) const {
    const Channel& c = ch_[static_cast<size_t>(ch)];
    if (!c.gate) {
        return c.mode != 0;  // modes 2/3 hold the output high while the gate is low
    }
    const uint64_t elapsed = now - c.start;
    switch (c.mode) {
    case 0:
        return elapsed >= c.reload;
    case 3:
        return elapsed % c.reload < (c.reload + 1) / 2;
    case 2:
        return elapsed % c.reload != c.reload - 1;  // low for one clock per period
    default:
        return true;
    }
}

uint64_t Pit::next_irq0_after(uint64_t now) const {
    const Channel& c = ch_[0];
    constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
    if (!c.gate || c.write_hi) {
        return kNever;
    }
    if (c.mode == 0) {
        const uint64_t at = c.start + c.reload;
        return at > now ? at : kNever;
    }
    if (c.mode == 2 || c.mode == 3) {
        // Rising edge at every multiple of the period after the count started.
        const uint64_t periods = (now - c.start) / c.reload + 1;
        return c.start + periods * c.reload;
    }
    return kNever;
}

uint8_t Pit::in8(uint16_t port, uint64_t now) {
    if (port == 0x43) {
        return 0xFF;  // the 8253 control word register is write-only
    }
    Channel& c = ch_[port & 3];
    const uint16_t value = c.latched ? c.latch : count(port & 3, now);
    uint8_t result;
    if (c.access == 1) {
        result = static_cast<uint8_t>(value);
        c.latched = false;
    } else if (c.access == 2) {
        result = static_cast<uint8_t>(value >> 8);
        c.latched = false;
    } else {
        result = static_cast<uint8_t>(c.read_hi ? value >> 8 : value);
        if (c.read_hi) {
            c.latched = false;
        }
        c.read_hi = !c.read_hi;
    }
    return result;
}

void Pit::out8(uint16_t port, uint8_t value, uint64_t now) {
    if (port == 0x43) {
        const int sel = value >> 6;
        if (sel == 3) {
            return;  // read-back command is 8254-only
        }
        Channel& c = ch_[static_cast<size_t>(sel)];
        const uint8_t access = (value >> 4) & 3;
        if (access == 0) {  // counter latch
            if (!c.latched) {
                c.latch = count(sel, now);
                c.latched = true;
                c.read_hi = false;
            }
            return;
        }
        c.access = access;
        c.mode = (value >> 1) & 7;
        if (c.mode > 5) {
            c.mode -= 4;  // modes 6/7 alias 2/3
        }
        c.write_hi = false;
        c.read_hi = false;
        c.latched = false;
        return;
    }
    Channel& c = ch_[port & 3];
    auto load = [&](uint16_t v) {
        c.reload = v == 0 ? 65536u : v;
        c.start = now;
    };
    if (c.access == 1) {
        load(value);
    } else if (c.access == 2) {
        load(static_cast<uint16_t>(value << 8));
    } else if (!c.write_hi) {
        c.pending_low = value;
        c.write_hi = true;  // counting is suspended until the high byte arrives
    } else {
        load(static_cast<uint16_t>(c.pending_low | (value << 8)));
        c.write_hi = false;
    }
}

void Pit::set_gate2(bool gate, uint64_t now) {
    Channel& c = ch_[2];
    if (gate && !c.gate) {
        c.start = now;
    }
    c.gate = gate;
}

} // namespace vette::host
