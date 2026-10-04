#include "host/pic.h"

namespace vette::host {

int Pic::next_irq() const {
    const uint8_t pending = static_cast<uint8_t>(irr_ & ~imr_);
    for (int irq = 0; irq < 8; ++irq) {
        const uint8_t bit = static_cast<uint8_t>(1u << irq);
        if (isr_ & bit) {
            return -1;  // an equal or higher priority interrupt is still in service
        }
        if (pending & bit) {
            return irq;
        }
    }
    return -1;
}

uint8_t Pic::irq_acknowledge() {
    const int irq = next_irq();
    if (irq < 0) {
        return static_cast<uint8_t>(base_ + 7);  // spurious IRQ7, as on real hardware
    }
    const uint8_t bit = static_cast<uint8_t>(1u << irq);
    irr_ &= static_cast<uint8_t>(~bit);
    isr_ |= bit;
    return static_cast<uint8_t>(base_ + irq);
}

uint8_t Pic::in8(uint16_t port) const {
    if (port == 0x21) {
        return imr_;
    }
    return read_isr_ ? isr_ : irr_;
}

void Pic::out8(uint16_t port, uint8_t value) {
    if (port == 0x20) {
        if (value & 0x10) {  // ICW1
            icw_step_ = 2;
            icw4_needed_ = (value & 0x01) != 0;
            imr_ = 0;
            isr_ = 0;
            read_isr_ = false;
        } else if ((value & 0x18) == 0x00) {  // OCW2
            if ((value & 0xE0) == 0x20) {          // non-specific EOI: clear highest-priority ISR bit
                for (int irq = 0; irq < 8; ++irq) {
                    if (isr_ & (1u << irq)) {
                        isr_ &= static_cast<uint8_t>(~(1u << irq));
                        break;
                    }
                }
            } else if ((value & 0xE0) == 0x60) {   // specific EOI
                isr_ &= static_cast<uint8_t>(~(1u << (value & 7)));
            }
        } else if ((value & 0x18) == 0x08) {  // OCW3
            if (value & 0x02) {
                read_isr_ = (value & 0x01) != 0;
            }
        }
        return;
    }
    // port 21h
    if (icw_step_ == 2) {
        base_ = static_cast<uint8_t>(value & 0xF8);
        icw_step_ = 3;  // single mode, so ICW3 is skipped; ICW4 follows if requested
        if (!icw4_needed_) {
            icw_step_ = 0;
        }
    } else if (icw_step_ == 3) {
        icw_step_ = 0;  // ICW4 (8086 mode etc.): nothing to model
    } else {
        imr_ = value;   // OCW1
    }
}

} // namespace vette::host
