// The 8250/16450 UART (host/uart.h) and the serial links (host/loopback_link.h): registers, transmit and
// receive timing, interrupts, loopback, modem status; the machine's serial ports and the in-memory cable.
// No game files needed.

#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

#include "host/loopback_link.h"
#include "host/machine.h"
#include "host/uart.h"
#include "test.h"

using namespace vette::host;

namespace {

// The other end of the cable, scripted.
class FakeLink final : public SerialLink {
public:
    void send(std::span<const uint8_t> bytes) override { sent.insert(sent.end(), bytes.begin(), bytes.end()); }
    size_t receive(std::span<uint8_t> out) override {
        size_t n = 0;
        while (n < out.size() && !incoming.empty()) {
            out[n++] = incoming.front();
            incoming.pop_front();
        }
        ++polls;
        return n;
    }
    bool connected() const override { return present; }

    std::vector<uint8_t> sent;
    std::deque<uint8_t> incoming;
    bool present = true;
    int polls = 0;
};

constexpr uint64_t kUs = 1000;

// 57600 baud 8N1 (VETTE's fastest setting: divisor 2), interrupts on for data and THRE, DTR/RTS/OUT2.
void open_57600(Uart& u, uint64_t t) {
    u.write(Uart::kLcr, 0x80, t);
    u.write(Uart::kData, 2, t);
    u.write(Uart::kIer, 0, t);
    u.write(Uart::kLcr, 0x03, t);
    u.write(Uart::kMcr, 0x0F, t);
}

} // namespace

TEST(uart_registers) {
    Uart u;
    CHECK_EQ(u.read(Uart::kIir, 0), 0x01);  // no interrupt pending
    CHECK_EQ(u.read(Uart::kLsr, 0), 0x60);  // THR and the transmitter empty
    u.write(Uart::kLcr, 0x80, 0);           // DLAB: the divisor latch
    u.write(Uart::kData, 0x60, 0);          // 1200 baud
    u.write(Uart::kIer, 0x00, 0);
    CHECK_EQ(u.read(Uart::kData, 0), 0x60);
    CHECK_EQ(u.read(Uart::kIer, 0), 0x00);
    u.write(Uart::kLcr, 0x03, 0);
    CHECK_EQ(u.read(Uart::kLcr, 0), 0x03);
    CHECK_EQ(u.char_ns(), 10 * 1'000'000'000ull * 0x60 / 115200);  // 10 bits at 1200 baud
    u.write(Uart::kLcr, 0x1F, 0);  // 8 bits, two stop bits, even parity: 12 bits
    CHECK_EQ(u.char_ns(), 12 * 1'000'000'000ull * 0x60 / 115200);
    u.write(Uart::kScratch, 0xA5, 0);
    CHECK_EQ(u.read(Uart::kScratch, 0), 0xA5);
    u.write(Uart::kMcr, 0xFF, 0);
    CHECK_EQ(u.read(Uart::kMcr, 0), 0x1F);  // bits 5-7 read 0
    u.write(Uart::kIir, 0xC7, 0);           // FCR: a 16450 has no FIFO
    CHECK_EQ(u.read(Uart::kIir, 0) & 0xC0, 0);
}

TEST(uart_transmit_timing_and_thre_interrupt) {
    Uart u;
    FakeLink link;
    u.set_link(&link);
    open_57600(u, 0);
    const uint64_t c = u.char_ns();
    CHECK_EQ(c, 173'611u);  // 10 bits at 57600
    u.write(Uart::kIer, 0x02, 0);  // THRE interrupts: THR is empty, so one is raised at once
    CHECK(u.interrupt());
    CHECK_EQ(u.read(Uart::kIir, 0), 0x02);
    CHECK(!u.interrupt());  // reading IIR cleared it
    CHECK_EQ(u.read(Uart::kIir, 0), 0x01);

    u.write(Uart::kData, 'A', 10 * kUs);  // into the shift register: THR empty again
    CHECK(u.interrupt());
    CHECK_EQ(u.read(Uart::kLsr, 10 * kUs) & 0x60, 0x20);  // THRE, not TEMT
    u.write(Uart::kData, 'B', 11 * kUs);  // waits in THR
    CHECK(!u.interrupt());
    CHECK_EQ(u.read(Uart::kLsr, 11 * kUs) & 0x60, 0x00);
    CHECK_EQ(u.next_event_ns(), 10 * kUs + c);
    u.advance(10 * kUs + c - 1);
    u.flush();
    CHECK(link.sent.empty());
    u.advance(10 * kUs + c);  // 'A' out; 'B' into the shift register: THRE
    u.flush();
    CHECK_EQ(link.sent.size(), 1u);
    CHECK(u.interrupt());
    u.advance(10 * kUs + 2 * c);
    u.flush();
    CHECK_EQ(link.sent.size(), 2u);
    CHECK_EQ(link.sent[1], 'B');
    CHECK_EQ(u.read(Uart::kLsr, 10 * kUs + 2 * c) & 0x60, 0x60);
    CHECK_EQ(u.stats().sent, 2u);
    // OUT2 off: the interrupt doesn't reach the PIC.
    u.write(Uart::kData, 'C', 1'000 * kUs);
    CHECK(u.interrupt());
    u.write(Uart::kMcr, 0x03, 1'000 * kUs);
    CHECK(!u.interrupt());
}

TEST(uart_receive_paced_without_overrun) {
    Uart u;
    FakeLink link;
    u.set_link(&link);
    for (uint8_t b : {'I', 'D', 'N'}) {
        link.incoming.push_back(b);
    }
    u.write(Uart::kLcr, 0x03, 0);
    u.advance(5 * kUs);
    CHECK_EQ(u.read(Uart::kLsr, 5 * kUs) & 1, 0);  // DTR off: the port isn't open, nothing taken
    CHECK_EQ(link.incoming.size(), 3u);
    open_57600(u, 10 * kUs);
    u.write(Uart::kIer, 0x01, 10 * kUs);
    u.advance(10 * kUs);
    CHECK(u.interrupt());
    CHECK_EQ(u.read(Uart::kIir, 10 * kUs), 0x04);
    CHECK_EQ(u.read(Uart::kData, 10 * kUs), 'I');
    CHECK(!u.interrupt());
    const uint64_t c = u.char_ns();
    // The next byte a character time later, not before.
    u.advance(10 * kUs + c - 1);
    CHECK_EQ(u.read(Uart::kLsr, 10 * kUs + c - 1) & 1, 0);
    CHECK_EQ(u.next_event_ns(), 10 * kUs + c);
    u.advance(10 * kUs + c);
    CHECK_EQ(u.read(Uart::kLsr, 10 * kUs + c) & 1, 1);
    // Read late: the third byte waits for it (no overrun, nothing lost), then follows a character later.
    const uint64_t late = 10 * kUs + 20 * c;
    u.advance(late);
    CHECK_EQ(u.read(Uart::kLsr, late) & 0x03, 0x01);
    CHECK_EQ(u.read(Uart::kData, late), 'D');
    u.advance(late + c);
    CHECK_EQ(u.read(Uart::kData, late + c), 'N');
    CHECK_EQ(u.stats().received, 3u);
    // Nothing more: the link is looked at again every kPollNs.
    const uint64_t t0 = late + 2 * c;
    u.advance(t0);
    const int polls = link.polls;
    u.advance(t0 + Uart::kPollNs - 1);
    CHECK_EQ(link.polls, polls);
    CHECK_EQ(u.next_event_ns(), t0 + Uart::kPollNs);
    u.advance(t0 + Uart::kPollNs);
    CHECK_EQ(link.polls, polls + 1);
}

TEST(uart_loopback_and_modem_status) {
    Uart u;
    FakeLink link;
    u.set_link(&link);
    u.write(Uart::kLcr, 0x03, 0);
    u.write(Uart::kMcr, 0x0B, 0);  // DTR, RTS, OUT2
    CHECK_EQ(u.read(Uart::kMsr, 0) & 0xF0, 0xB0);  // the peer's there: CTS, DSR, DCD
    u.write(Uart::kIer, 0x08, 0);
    link.present = false;
    u.advance(kUs);
    CHECK(u.interrupt());
    CHECK_EQ(u.read(Uart::kIir, kUs), 0x00);
    CHECK_EQ(u.read(Uart::kMsr, kUs), 0x0B);  // lines down, DCTS, DDSR, DDCD
    CHECK(!u.interrupt());
    // Loopback: what's sent comes back, nothing reaches the link; MSR mirrors MCR.
    u.write(Uart::kIer, 0x00, kUs);
    u.write(Uart::kMcr, 0x1E, kUs);  // loop, RTS, OUT1, OUT2
    CHECK_EQ(u.read(Uart::kMsr, kUs) & 0xF0, 0xD0);
    u.write(Uart::kData, 0x5A, 2 * kUs);
    u.advance(2 * kUs + u.char_ns());
    u.flush();
    CHECK_EQ(u.read(Uart::kData, 2 * kUs + u.char_ns()), 0x5A);
    CHECK(link.sent.empty());
}

TEST(serial_link_loopback_cable_and_delay) {
    LoopbackCable cable;
    const uint8_t a[] = {1, 2, 3};
    cable.end(0).send(a);
    uint8_t buf[8] = {};
    CHECK_EQ(cable.end(1).receive(buf), 3u);
    CHECK_EQ(buf[2], 3);
    CHECK_EQ(cable.end(0).receive(buf), 0u);
    cable.set_plugged(false);
    CHECK(!cable.end(0).connected());
    cable.end(0).send(a);
    cable.set_plugged(true);
    CHECK_EQ(cable.end(1).receive(buf), 0u);  // sent while unplugged: lost

    uint64_t now = 0;
    DelayedLink::Options opt;
    opt.delay_ns = 50'000'000;
    opt.jitter_ns = 100'000'000;
    DelayedLink delayed(cable.end(1), [&now] { return now; }, opt);
    // A byte a millisecond for 20 ms: each arrives 50-150 ms after it was sent, all in order.
    std::vector<std::pair<uint8_t, uint64_t>> got;  // (value, arrival)
    for (now = 0; now < 300'000'000; now += 1'000'000) {
        if (now < 20'000'000) {
            const uint8_t v[] = {static_cast<uint8_t>(now / 1'000'000)};
            cable.end(0).send(v);
        }
        const size_t n = delayed.receive(buf);
        for (size_t i = 0; i < n; ++i) {
            got.emplace_back(buf[i], now);
        }
    }
    CHECK_EQ(got.size(), 20u);
    for (size_t i = 0; i < got.size(); ++i) {
        CHECK_EQ(got[i].first, static_cast<uint8_t>(i));
        const uint64_t sent_at = i * 1'000'000;
        CHECK(got[i].second >= sent_at + opt.delay_ns);
        CHECK(i == 0 || got[i].second >= got[i - 1].second);
    }
    CHECK(got.back().second <= 19'000'000 + opt.delay_ns + opt.jitter_ns);
    CHECK_EQ(delayed.in_flight(), 0u);
}

// Without a link the PC has no serial ports, as before: the BIOS lists none, the ports float.
TEST(machine_serial_ports_only_with_a_link) {
    MachineConfig config;
    config.game_dir = ".";
    Machine plain(config);
    CHECK(!plain.has_serial());
    CHECK_EQ(plain.in8(0x3FD), 0xFF);
    CHECK_EQ(plain.in8(0x2FA), 0xFF);
    CHECK_EQ(plain.memory().read16(0x400), 0);

    Machine linked(config);
    LoopbackCable cable;
    linked.attach_serial(&cable.end(0));
    CHECK(linked.has_serial());
    CHECK_EQ(linked.memory().read16(0x400), 0x3F8);
    CHECK_EQ(linked.memory().read16(0x402), 0x2F8);
    CHECK_EQ((linked.memory().read16(0x410) >> 9) & 7, 2);
    CHECK_EQ(linked.in8(0x3FD), 0x60);
    CHECK_EQ(linked.in8(0x2FA), 0x01);
    // The cable goes to the port the program opens: bytes sent on COM2 reach the other end.
    linked.out8(0x2FB, 0x03);
    linked.out8(0x2FC, 0x0B);
    linked.out8(0x2F8, 0x42);
    linked.memory().write8(0, 0xF4);  // (no program: the CPU halts at its reset address with IF clear)
    linked.run_for(5'000'000);  // a character at 9600 baud: 1.04 ms
    uint8_t buf[4] = {};
    CHECK_EQ(cable.end(1).receive(buf), 1u);
    CHECK_EQ(buf[0], 0x42);
}
