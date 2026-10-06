#pragma once
// National Semiconductor 8250/16450 UART (a PC's COM port), connected to a SerialLink: the two-player
// cable (re/notes/12-two-player.md). Modelled as VETTE.EXE's serial library (422F) uses it: THR/RBR,
// IER, IIR, LCR (word length, parity, stop bits, DLAB), the divisor latch, MCR (DTR, RTS, OUT1, OUT2,
// loopback), LSR, MSR and the scratch register; no FIFO (a 16450: IIR bits 6-7 read 0, FCR writes are
// ignored).
//
// Timing is in emulated nanoseconds. A byte written to THR moves to the transmit shift register at
// once if it is idle (THR empty again: the THRE interrupt) and is on the line one character time later
// (start bit, data, parity, stop bits at the divisor's baud rate); the line's bytes go to the link on
// flush(). Received bytes are paced the same way, one per character time at the configured rate, and
// never overrun: the next byte waits until the program has read RBR (as with hardware flow control), so
// nothing the peer sent is lost however late the program reads. Bytes are taken from the link only
// while DTR is on (the program has opened the port). The modem status lines follow the link: CTS, DSR
// and DCD are on while the peer is connected (a null-modem cable's DTR/RTS of the other side), RI off.
//
// The interrupt output is gated by MCR OUT2, as on a PC's serial card; interrupt() is the IRQ line.

#include <cstdint>
#include <deque>
#include <vector>

#include "host/serial_link.h"

namespace vette::host {

class Uart {
public:
    static constexpr uint64_t kNever = UINT64_MAX;
    static constexpr uint32_t kClockHz = 115200;  // 1.8432 MHz / 16: the baud rate at divisor 1
    // With nothing to deliver, the link is looked at again this often (emulated time).
    static constexpr uint64_t kPollNs = 500'000;

    // Registers by offset from the base port.
    enum Reg : uint8_t { kData = 0, kIer = 1, kIir = 2, kLcr = 3, kMcr = 4, kLsr = 5, kMsr = 6, kScratch = 7 };

    void set_link(SerialLink* link) { link_ = link; }
    SerialLink* link() const { return link_; }

    uint8_t read(int reg, uint64_t now_ns);
    void write(int reg, uint8_t value, uint64_t now_ns);

    // Completes the transmissions and delivers the received bytes due by `now_ns`.
    void advance(uint64_t now_ns);
    // When advance() next has something to do (kNever: nothing until a register is accessed).
    uint64_t next_event_ns() const;
    // The interrupt request line (INTRPT AND OUT2).
    bool interrupt() const;
    // Sends what has been transmitted since the last flush to the link.
    void flush();

    bool dtr() const { return (mcr_ & 1) != 0; }
    // One character's time on the line at the current settings.
    uint64_t char_ns() const;

    struct Stats {
        uint64_t sent = 0, received = 0;  // bytes, line side
    };
    const Stats& stats() const { return stats_; }

private:
    bool loop() const { return (mcr_ & 0x10) != 0; }
    bool receiving() const { return link_ && dtr() && !loop(); }
    bool peer_present() const;
    uint8_t data_mask() const { return static_cast<uint8_t>((1u << (5 + (lcr_ & 3))) - 1); }
    uint8_t msr_lines() const;     // MSR bits 4-7
    void update_msr();              // latches the delta bits for line changes
    void line_byte(uint8_t value);  // a byte has gone out on the line
    void receive_byte(uint8_t value, uint64_t now_ns);
    bool lsr_error() const { return (lsr_err_ & 0x1E) != 0; }

    SerialLink* link_ = nullptr;
    // Registers.
    uint8_t ier_ = 0, lcr_ = 0, mcr_ = 0, scratch_ = 0;
    uint8_t dll_ = 0x0C, dlm_ = 0;  // 9600 baud, as a BIOS leaves it
    // Transmitter: THR, and the shift register putting `tsr_` on the line until `tsr_done_`.
    uint8_t thr_ = 0, tsr_ = 0;
    bool thr_full_ = false, tsr_busy_ = false;
    uint64_t tsr_done_ = 0;
    bool thre_pending_ = false;  // the THRE interrupt: set when THR empties, cleared by a THR write or IIR read
    // Receiver: RBR, and the next byte not before `rx_ready_` (one character after the last).
    uint8_t rbr_ = 0;
    bool data_ready_ = false;
    uint8_t lsr_err_ = 0;  // OE, PE, FE, BI (bits 1-4), cleared by reading LSR
    uint64_t rx_ready_ = 0, next_poll_ = 0;
    std::deque<uint8_t> staged_;  // taken from the link, not yet in RBR
    // Modem status.
    uint8_t msr_delta_ = 0;       // DCTS, DDSR, TERI, DDCD
    uint8_t msr_last_ = 0;        // lines (bits 4-7) as last latched
    std::vector<uint8_t> tx_out_;  // on the line since the last flush
    Stats stats_;
};

} // namespace vette::host
