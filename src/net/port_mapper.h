#pragma once
// Opens a TCP port on the home router for a direct connection, and closes it again: UPnP IGD
// (miniupnpc), else NAT-PMP (libnatpmp), else PCP (net/nat.h). Also asks STUN for the public address,
// then judges whether a friend can connect (net/nat.h, judge_reachability). Works on its own thread; the
// mapping is renewed while it lasts and removed when the PortMapper is destroyed (and expires after an
// hour if the program dies).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "net/nat.h"

namespace vette::net {

class PortMapper {
public:
    struct Result {
        RouterMapping mapping;
        std::optional<std::uint32_t> stun_ip;
        Reachability reachability;
    };

    // Starts mapping `port` (TCP, to this computer, same external port if the router agrees).
    // `manual_forward`: the player has forwarded the port themselves (see judge_reachability).
    // `use_stun`: also ask the STUN servers (outside machines) for the public address; without, the
    // router's word is all there is.
    explicit PortMapper(std::uint16_t port, bool manual_forward = false, bool use_stun = true);
    ~PortMapper();  // removes the mapping
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    // Null until the router and STUN have answered (a few seconds at most).
    std::optional<Result> result() const;
    // Removes the mapping now (also done by the destructor).
    void remove();

    // Whether this build can map ports at all (built with miniupnpc / libnatpmp).
    static bool available();

private:
    void run();
    bool map_upnp(RouterMapping& m);
    bool map_natpmp(RouterMapping& m);
    bool map_pcp(RouterMapping& m);
    void renew();
    void unmap();

    std::uint16_t port_;
    bool manual_forward_;
    bool use_stun_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Result> result_;
    std::atomic<bool> stop_{false};
    bool removed_ = false;
    std::thread thread_;

    // What has to be undone (UPnP control URL and service, the gateway, the PCP nonce).
    struct State;
    State* state_ = nullptr;
};

}  // namespace vette::net
