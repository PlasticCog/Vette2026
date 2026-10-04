#pragma once
// Intel 8259A interrupt controller (single, master only: IRQ0-7), ports 20h/21h.

#include <cstdint>

#include "host/cpu.h"

namespace vette::host {

class Pic final : public InterruptController {
public:
    void raise(int irq) { irr_ |= static_cast<uint8_t>(1u << irq); }
    void lower(int irq) { irr_ &= static_cast<uint8_t>(~(1u << irq)); }

    bool irq_pending() const override { return next_irq() >= 0; }
    uint8_t irq_acknowledge() override;

    uint8_t in8(uint16_t port) const;
    void out8(uint16_t port, uint8_t value);

    uint8_t mask() const { return imr_; }
    uint8_t in_service() const { return isr_; }

private:
    int next_irq() const;

    uint8_t irr_ = 0;    // requested
    uint8_t isr_ = 0;    // in service
    uint8_t imr_ = 0;    // masked
    uint8_t base_ = 8;   // vector of IRQ0 (BIOS default)
    int icw_step_ = 0;   // >0 while an ICW2..ICW4 initialization sequence is in progress
    bool icw4_needed_ = false;
    bool read_isr_ = false;  // OCW3 read register select
};

} // namespace vette::host
