#include "sound/fm_chip.h"

#include <array>

#include "ymfm_opl.h"
#include "ymfm_opn.h"

namespace vette::sound {

// ymfm talks to the outside through this interface: timers, IRQ and busy flag. The timers are run
// here in chip clocks, advanced as samples are generated. A ymfm chip keeps pointers to itself and to
// this interface, so neither may move: the chip is built in place, after the interface.
struct FmChip::Impl final : ymfm::ymfm_interface {
    std::unique_ptr<ymfm::ym3812> opl;
    std::unique_ptr<ymfm::ym2203> opn;
    uint32_t clocks_per_sample = 1;
    std::array<int64_t, 2> timer{-1, -1};  // clocks until expiry, -1 = stopped
    bool irq_line = false;

    // Resampler state: the last two chip samples and the position between them.
    float prev = 0, next = 0;
    double phase = 1;  // in chip samples
    double step = 1;   // chip samples per output sample

    explicit Impl(Type type) {
        if (type == Type::Ym3812) {
            opl = std::make_unique<ymfm::ym3812>(*this);
        } else {
            opn = std::make_unique<ymfm::ym2203>(*this);
        }
    }

    template <typename F>
    decltype(auto) with_chip(F&& f) {
        return opl ? f(*opl) : f(*opn);
    }

    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        if (tnum < timer.size()) {
            timer[tnum] = duration_in_clocks;
        }
    }
    void ymfm_update_irq(bool asserted) override { irq_line = asserted; }

    template <typename Chip>
    static float generate(Chip& c) {
        typename Chip::output_data out;
        c.generate(&out, 1);
        int32_t sum = 0;
        for (const int32_t v : out.data) {
            sum += v;
        }
        return static_cast<float>(sum) / 32768.0f;
    }

    float generate_one() {
        // Timers first: an expiry can change the chip's state (flags, IRQ).
        for (uint32_t t = 0; t < timer.size(); ++t) {
            if (timer[t] >= 0) {
                timer[t] -= clocks_per_sample;
                if (timer[t] < 0) {
                    timer[t] = -1;
                    m_engine->engine_timer_expired(t);  // re-arms the timer if it's still enabled
                }
            }
        }
        return opl ? generate(*opl) : generate(*opn);
    }
};

FmChip::FmChip(Type type, int output_rate, uint32_t clock)
    : impl_(std::make_unique<Impl>(type)), type_(type), output_rate_(output_rate) {
    if (clock == 0) {
        clock = type == Type::Ym3812 ? kAdLibClock : kPc98Clock;
    }
    chip_rate_ = impl_->with_chip([clock](auto& c) { return c.sample_rate(clock); });
    impl_->clocks_per_sample = clock / chip_rate_;
    impl_->step = static_cast<double>(chip_rate_) / output_rate_;
    reset();
}

FmChip::~FmChip() = default;

void FmChip::reset() {
    impl_->with_chip([](auto& c) { c.reset(); });
    impl_->timer = {-1, -1};
    impl_->prev = impl_->next = 0;
    impl_->phase = 1;
}

void FmChip::write(uint8_t reg, uint8_t value) {
    impl_->with_chip([&](auto& c) {
        c.write_address(reg);
        c.write_data(value);
    });
}

uint8_t FmChip::status() {
    return impl_->with_chip([](auto& c) { return c.read_status(); });
}

bool FmChip::irq() const { return impl_->irq_line; }

void FmChip::render(float* out, int frames) {
    Impl& s = *impl_;
    for (int i = 0; i < frames; ++i) {
        while (s.phase >= 1) {
            s.prev = s.next;
            s.next = s.generate_one();
            s.phase -= 1;
        }
        out[i] = s.prev + (s.next - s.prev) * static_cast<float>(s.phase);
        s.phase += s.step;
    }
}

}  // namespace vette::sound
