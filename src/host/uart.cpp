#include "host/uart.h"

#include <algorithm>
#include <array>

namespace vette::host {

uint64_t Uart::char_ns() const {
    // Start bit, 5-8 data bits, parity, and 1, 1.5 (5-bit words) or 2 stop bits, counted in half bits.
    const uint64_t data = 5 + (lcr_ & 3u);
    const uint64_t stop = (lcr_ & 4) ? ((lcr_ & 3) == 0 ? 3 : 4) : 2;
    const uint64_t half_bits = 2 * (1 + data + ((lcr_ & 8) ? 1 : 0)) + stop;
    const uint64_t divisor = (static_cast<uint64_t>(dlm_) << 8 | dll_) == 0 ? 0x10000 : (static_cast<uint64_t>(dlm_) << 8 | dll_);
    return half_bits * divisor * 1'000'000'000 / (2ull * kClockHz);
}

bool Uart::peer_present() const { return link_ && link_->connected(); }

uint8_t Uart::msr_lines() const {
    if (loop()) {  // CTS = RTS, DSR = DTR, RI = OUT1, DCD = OUT2
        return static_cast<uint8_t>(((mcr_ & 2) ? 0x10 : 0) | ((mcr_ & 1) ? 0x20 : 0) | ((mcr_ & 4) ? 0x40 : 0) |
                                    ((mcr_ & 8) ? 0x80 : 0));
    }
    return peer_present() ? 0xB0 : 0x00;  // a null-modem cable: CTS, DSR and DCD from the peer
}

void Uart::update_msr() {
    const uint8_t lines = msr_lines();
    const uint8_t changed = static_cast<uint8_t>(lines ^ msr_last_);
    if (changed & 0x10) msr_delta_ |= 0x01;                     // DCTS
    if (changed & 0x20) msr_delta_ |= 0x02;                     // DDSR
    if ((changed & 0x40) && !(lines & 0x40)) msr_delta_ |= 0x04;  // TERI: the end of a ring
    if (changed & 0x80) msr_delta_ |= 0x08;                     // DDCD
    msr_last_ = lines;
}

void Uart::receive_byte(uint8_t value, uint64_t now_ns) {
    if (data_ready_) {
        lsr_err_ |= 0x02;  // overrun (only in loopback: the link waits for RBR to be read)
    }
    rbr_ = static_cast<uint8_t>(value & data_mask());
    data_ready_ = true;
    rx_ready_ = now_ns + char_ns();
    ++stats_.received;
}

void Uart::line_byte(uint8_t value) {
    ++stats_.sent;
    if (loop()) {
        receive_byte(value, tsr_done_);
    } else if (link_) {
        tx_out_.push_back(value);
    }
}

void Uart::advance(uint64_t now_ns) {
    // Transmitter: the shift register's byte is out; THR's moves in (THR empty: THRE).
    while (tsr_busy_ && tsr_done_ <= now_ns) {
        line_byte(tsr_);
        if (thr_full_) {
            tsr_ = thr_;
            thr_full_ = false;
            tsr_done_ += char_ns();
            thre_pending_ = true;
        } else {
            tsr_busy_ = false;
        }
    }
    // Receiver: the next byte from the link, a character time after the last, once RBR has been read.
    if (receiving() && !data_ready_) {
        if (staged_.empty() && now_ns >= next_poll_) {
            std::array<uint8_t, 64> buf;
            const size_t n = link_->receive(buf);
            staged_.insert(staged_.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(std::min(n, buf.size())));
            if (n == 0) {
                next_poll_ = now_ns + kPollNs;
            }
        }
        if (!staged_.empty() && now_ns >= rx_ready_) {
            const uint8_t b = staged_.front();
            staged_.pop_front();
            receive_byte(b, now_ns);
        }
    }
    update_msr();
}

uint64_t Uart::next_event_ns() const {
    uint64_t t = tsr_busy_ ? tsr_done_ : kNever;
    if (receiving() && !data_ready_) {
        t = std::min(t, staged_.empty() ? std::max(rx_ready_, next_poll_) : rx_ready_);
    }
    return t;
}

bool Uart::interrupt() const {
    const bool pending = ((ier_ & 4) && lsr_error()) || ((ier_ & 1) && data_ready_) ||
                         ((ier_ & 2) && thre_pending_) || ((ier_ & 8) && msr_delta_ != 0);
    return pending && (mcr_ & 8) != 0;
}

void Uart::flush() {
    if (link_ && !tx_out_.empty()) {
        link_->send(tx_out_);
    }
    tx_out_.clear();
}

uint8_t Uart::read(int reg, uint64_t now_ns) {
    advance(now_ns);
    const bool dlab = (lcr_ & 0x80) != 0;
    switch (reg & 7) {
    case kData:
        if (dlab) {
            return dll_;
        }
        data_ready_ = false;
        return rbr_;
    case kIer:
        return dlab ? dlm_ : ier_;
    case kIir:
        if ((ier_ & 4) && lsr_error()) {
            return 0x06;
        }
        if ((ier_ & 1) && data_ready_) {
            return 0x04;
        }
        if ((ier_ & 2) && thre_pending_) {
            thre_pending_ = false;  // reading IIR with THRE the source clears it
            return 0x02;
        }
        if ((ier_ & 8) && msr_delta_) {
            return 0x00;
        }
        return 0x01;
    case kLcr:
        return lcr_;
    case kMcr:
        return mcr_;
    case kLsr: {
        const auto v = static_cast<uint8_t>((data_ready_ ? 0x01 : 0) | lsr_err_ | (thr_full_ ? 0 : 0x20) |
                                            (!thr_full_ && !tsr_busy_ ? 0x40 : 0));
        lsr_err_ = 0;
        return v;
    }
    case kMsr: {
        const auto v = static_cast<uint8_t>(msr_last_ | msr_delta_);
        msr_delta_ = 0;
        return v;
    }
    default:
        return scratch_;
    }
}

void Uart::write(int reg, uint8_t value, uint64_t now_ns) {
    advance(now_ns);
    const bool dlab = (lcr_ & 0x80) != 0;
    switch (reg & 7) {
    case kData:
        if (dlab) {
            dll_ = value;
            break;
        }
        thre_pending_ = false;
        value = static_cast<uint8_t>(value & data_mask());
        if (!tsr_busy_) {  // straight into the shift register: THR is empty again at once
            tsr_ = value;
            tsr_busy_ = true;
            tsr_done_ = now_ns + char_ns();
            thre_pending_ = true;
        } else {
            thr_ = value;  // (over a byte still waiting there, as the hardware does)
            thr_full_ = true;
        }
        break;
    case kIer:
        if (dlab) {
            dlm_ = value;
            break;
        }
        ier_ = static_cast<uint8_t>(value & 0x0F);
        if ((ier_ & 2) && !thr_full_) {
            thre_pending_ = true;  // enabling THRE interrupts with THR empty raises one
        }
        break;
    case kIir:  // FCR on a 16550; a 16450 has no FIFO
        break;
    case kLcr:
        lcr_ = value;
        break;
    case kMcr:
        mcr_ = static_cast<uint8_t>(value & 0x1F);
        break;
    case kScratch:
        scratch_ = value;
        break;
    default:  // LSR, MSR: read-only (factory test)
        break;
    }
    update_msr();
}

} // namespace vette::host
