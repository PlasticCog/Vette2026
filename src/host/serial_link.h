#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace vette::host {
// The other player's end of the serial cable: an ordered, reliable byte stream (whatever carries it).
// send() is called from the emulation thread with the bytes our UART transmitted; receive() is polled by
// the UART and returns what has arrived (non-blocking). connected() is false until the peer is there
// and after it has gone.
class SerialLink {
public:
    virtual ~SerialLink() = default;
    virtual void send(std::span<const std::uint8_t> bytes) = 0;
    virtual std::size_t receive(std::span<std::uint8_t> out) = 0;
    virtual bool connected() const = 0;
};
}
