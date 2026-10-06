// Online play: the reliable serial stream (resend and de-duplication) and the batching of bytes.

#include "net/session.h"
#include "net/stream.h"
#include "test.h"

using namespace vette::net;

namespace {

std::vector<std::uint8_t> seq(int from, int n) {
    std::vector<std::uint8_t> v;
    for (int i = 0; i < n; ++i) {
        v.push_back(static_cast<std::uint8_t>(from + i));
    }
    return v;
}

}  // namespace

TEST(net_stream_send_ack_rewind) {
    ReliableStream s(100);
    CHECK(s.write(seq(0, 30)));
    CHECK_EQ(s.unsent(), std::size_t{30});
    auto chunk = s.unsent_bytes(10);
    CHECK(chunk.size() == 10 && chunk[0] == 0 && chunk[9] == 9);
    s.mark_sent(10);
    chunk = s.unsent_bytes(100);
    CHECK(chunk.size() == 20 && chunk[0] == 10);
    s.mark_sent(20);
    CHECK_EQ(s.sent(), std::uint64_t{30});
    CHECK(s.on_ack(12));
    CHECK_EQ(s.acked(), std::uint64_t{12});
    CHECK(s.on_ack(5));    // stale: ignored
    CHECK_EQ(s.acked(), std::uint64_t{12});
    CHECK(!s.on_ack(31));  // beyond what was written
    s.rewind();            // a reconnect: everything unacknowledged again
    CHECK_EQ(s.sent(), std::uint64_t{12});
    chunk = s.unsent_bytes(100);
    CHECK(chunk.size() == 18 && chunk[0] == 12);
    // An acknowledgement beyond the rewound position moves it forward.
    CHECK(s.on_ack(20));
    CHECK_EQ(s.sent(), std::uint64_t{20});
    CHECK_EQ(s.unsent(), std::size_t{10});
    // The unacknowledged limit.
    CHECK(s.write(seq(30, 90)));    // 10 + 90 = 100 waiting
    CHECK(!s.write(seq(120, 1)));
    CHECK_EQ(s.written(), std::uint64_t{120});
}

TEST(net_stream_receive) {
    ReliableStream r;
    std::span<const std::uint8_t> fresh;
    const auto a = seq(0, 10);
    CHECK(r.receive(0, a, fresh) == ReliableStream::Received::Applied);
    CHECK(fresh.size() == 10 && r.received() == 10);
    CHECK(r.receive(0, a, fresh) == ReliableStream::Received::Duplicate);  // resent after a drop
    CHECK(fresh.empty());
    const auto overlap = seq(5, 10);  // 5..14: only 10..14 is new
    CHECK(r.receive(5, overlap, fresh) == ReliableStream::Received::Applied);
    CHECK(fresh.size() == 5 && fresh[0] == 10 && r.received() == 15);
    const auto later = seq(20, 5);  // 15..19 missing
    CHECK(r.receive(20, later, fresh) == ReliableStream::Received::Gap);
    CHECK(fresh.empty() && r.received() == 15);
}

TEST(net_stream_long_run_resends) {
    // Lots of data, every chunk lost once: the receiver ends up with exactly the stream.
    ReliableStream tx, rx;
    std::vector<std::uint8_t> sent, got;
    std::uint32_t lcg = 1;
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> bytes;
        for (int i = 0; i < 37; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            bytes.push_back(static_cast<std::uint8_t>(lcg >> 24));
        }
        CHECK(tx.write(bytes));
        sent.insert(sent.end(), bytes.begin(), bytes.end());
        auto chunk = tx.unsent_bytes(50);
        tx.mark_sent(chunk.size());  // lost
        tx.rewind();
        while (tx.unsent()) {
            chunk = tx.unsent_bytes(50);
            const auto offset = static_cast<std::uint32_t>(tx.sent());
            std::span<const std::uint8_t> fresh;
            rx.receive(offset, chunk, fresh);
            got.insert(got.end(), fresh.begin(), fresh.end());
            tx.mark_sent(chunk.size());
        }
        CHECK(tx.on_ack(static_cast<std::uint32_t>(rx.received())));
    }
    CHECK(got == sent);
    CHECK_EQ(tx.acked(), tx.written());
}

TEST(net_batching_due) {
    Batching b;  // gap 2 ms, hold 8 ms, interval 33.3 ms
    // Long after the last message: a burst goes when it pauses for 2 ms...
    CHECK_EQ(b.due(1'000'000, 1'000'500, 0), std::int64_t{1'002'500});
    // ...or after 8 ms if it doesn't pause...
    CHECK_EQ(b.due(1'000'000, 1'007'000, 0), std::int64_t{1'008'000});
    // ...but never sooner than 33.3 ms after the previous message.
    CHECK_EQ(b.due(1'000'000, 1'000'500, 990'000), std::int64_t{1'023'333});
}
