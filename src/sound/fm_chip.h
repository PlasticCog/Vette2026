#pragma once
// An FM sound chip, emulated by ymfm (BSD-3-Clause, Aaron Giles): the AdLib's YM3812 (OPL2) or the
// PC-98's YM2203 (OPN, 3 FM channels plus the 3-channel SSG). Register writes take effect from the
// next render(). The chip runs at its own rate and render() resamples it (linear) to the output rate.
// The chip's two timers run with it, so a hosted sound driver that waits on the timer flags or IRQ
// works (status(), irq()).

#include <cstdint>
#include <memory>

namespace vette::sound {

class FmChip {
public:
    enum class Type { Ym3812, Ym2203 };
    static constexpr uint32_t kAdLibClock = 3'579'545;  // YM3812 on an AdLib card
    static constexpr uint32_t kPc98Clock = 3'993'600;   // YM2203 on the PC-9801-26K sound board

    FmChip(Type type, int output_rate, uint32_t clock = 0);  // clock 0: the type's usual clock
    ~FmChip();
    FmChip(const FmChip&) = delete;
    FmChip& operator=(const FmChip&) = delete;

    Type type() const { return type_; }
    void reset();
    void write(uint8_t reg, uint8_t value);  // address then data, as on the card's two ports
    uint8_t status();                        // the status port: timer flags (and busy)
    bool irq() const;                        // the chip's IRQ line (timers, when enabled)

    // Mono samples at the output rate, nominally -1..1 (several channels at full volume can exceed it).
    void render(float* out, int frames);

    uint32_t chip_rate() const { return chip_rate_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Type type_;
    int output_rate_;
    uint32_t chip_rate_ = 0;
};

}  // namespace vette::sound
