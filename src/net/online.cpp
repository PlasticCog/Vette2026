#include "net/online.h"

#include <chrono>
#include <random>

#include "net/direct.h"
#include "net/port_mapper.h"
#include "net/room_link.h"

namespace vette::net {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string random_key() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) {
        out += kHex[rd() & 15];
    }
    return out;
}

bool finished(LinkState s) { return s == LinkState::Failed || s == LinkState::Closed; }

constexpr std::int64_t kOfferWaitMs = 4000;     // guest: the host's offer comes right after joining
constexpr std::int64_t kMappingWaitMs = 1500;   // host: how long an offer waits for the router
constexpr std::int64_t kDecisionWaitMs = 6000;  // host: on top of the guest's attempt

}  // namespace

const char* to_string(Route route) {
    switch (route) {
    case Route::None: return "";
    case Route::Server: return "via server";
    case Route::Direct: return "direct";
    }
    return "";
}

OnlineLink::OnlineLink(OnlineOptions options, bool host) : options_(std::move(options)), host_(host) {
    status_.host = host;
    if (host) {
        settings_ = options_.race_settings;
    }
}

std::unique_ptr<OnlineLink> OnlineLink::host(OnlineOptions options) {
    std::unique_ptr<OnlineLink> link(new OnlineLink(std::move(options), true));
    link->thread_ = std::thread([p = link.get()] { p->run(); });
    return link;
}

std::unique_ptr<OnlineLink> OnlineLink::join(OnlineOptions options, std::string_view pasted) {
    std::unique_ptr<OnlineLink> link(new OnlineLink(std::move(options), false));
    link->invite_ = parse_invite(pasted);
    if (!link->invite_) {
        link->failure_ = "That isn't a VETTE! 2026 code or invite link.";
    } else if (link->invite_->kind == Invite::Kind::Room && link->options_.server_url.empty()) {
        link->failure_ = "Room codes need the online server, and none is set.";
    }
    link->thread_ = std::thread([p = link.get()] { p->run(); });
    return link;
}

OnlineLink::~OnlineLink() {
    close();
    if (thread_.joinable()) {
        thread_.join();
    }
    for (auto& t : cleanups_) {
        t.join();
    }
    active_ = nullptr;
}

void OnlineLink::send(std::span<const std::uint8_t> bytes) {
    if (host::SerialLink* link = active_.load()) {
        link->send(bytes);
    }
}

std::size_t OnlineLink::receive(std::span<std::uint8_t> out) {
    host::SerialLink* link = active_.load();
    return link ? link->receive(out) : 0;
}

bool OnlineLink::connected() const {
    host::SerialLink* link = active_.load();
    return link && link->connected();
}

OnlineStatus OnlineLink::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

std::string OnlineLink::invite_url() const {
    std::lock_guard lock(mutex_);
    if (!host_) {
        return {};
    }
    if (!options_.server_url.empty() && status_.room.state == RouteStatus::State::Ready) {
        return invite_link(options_.server_url, {Invite::Kind::Room, status_.room.code});
    }
    if (status_.direct.state == RouteStatus::State::Ready) {
        return invite_link(options_.server_url, {Invite::Kind::Direct, status_.direct.code});
    }
    return {};
}

bool OnlineLink::set_race_settings(const RaceSettings& settings) {
    std::lock_guard lock(mutex_);
    if (!host_) {
        return false;
    }
    settings_ = settings;
    if (room_) {
        room_->set_race_settings(settings);
    }
    if (direct_host_) {
        direct_host_->set_race_settings(settings);
    }
    return true;
}

std::optional<RaceSettings> OnlineLink::race_settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

void OnlineLink::close() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

void OnlineLink::simulate_drop(int offline_ms) {
    std::lock_guard lock(mutex_);
    host::SerialLink* link = active_.load();
    if (room_ && link == room_.get()) {
        room_->simulate_drop(offline_ms);
    } else if (direct_join_ && link == direct_join_.get()) {
        direct_join_->simulate_drop(offline_ms);
    } else if (direct_host_ && link == direct_host_.get()) {
        direct_host_->simulate_drop();
    }
}

void OnlineLink::choose(Route route, host::SerialLink* link) {
    std::unique_ptr<RoomLink> room;
    std::unique_ptr<DirectHost> direct_host;
    std::unique_ptr<DirectJoin> direct_join;
    std::unique_ptr<PortMapper> mapper;
    // Closed on their own threads: the router may take a moment to answer, and so may the server.
    struct Retire {
        OnlineLink& self;
        std::unique_ptr<RoomLink>& room;
        std::unique_ptr<DirectHost>& direct_host;
        std::unique_ptr<DirectJoin>& direct_join;
        std::unique_ptr<PortMapper>& mapper;
        ~Retire() {
            if (room || direct_host || direct_join || mapper) {
                self.cleanups_.emplace_back([r = std::move(room), h = std::move(direct_host), j = std::move(direct_join),
                                             m = std::move(mapper)]() mutable {
                    r.reset();
                    h.reset();
                    j.reset();
                    m.reset();
                });
            }
        }
    } retire{*this, room, direct_host, direct_join, mapper};
    {
        std::lock_guard lock(mutex_);
        status_.route = route;
        active_ = link;
        // The other ways in are closed (outside the lock: closing takes a moment).
        if (route == Route::Direct) {
            if (host_) {
                room = std::move(room_);  // the host's room closes; the guest leaves its own soon
            }
        } else {
            direct_host = std::move(direct_host_);
            direct_join = std::move(direct_join_);
            mapper = std::move(mapper_);  // removes the router's port forwarding
        }
    }
}

void OnlineLink::publish_active() {
    host::SerialLink* link = active_.load();
    LinkStatus st;
    std::optional<RaceSettings> settings;
    {
        std::lock_guard lock(mutex_);
        if (link && link == room_.get()) {
            st = room_->status();
            settings = room_->race_settings();
        } else if (link && link == direct_host_.get()) {
            st = direct_host_->status();
        } else if (link && link == direct_join_.get()) {
            st = direct_join_->status();
            settings = direct_join_->race_settings();
        } else {
            return;
        }
        status_.link = st;
        status_.state = st.state;
        // The stream's echo needs messages both ways; the keepalive's ping says it meanwhile.
        status_.rtt_ms = st.rtt_ms >= 0 ? st.rtt_ms : st.server_rtt_ms;
        if (!host_ && settings) {
            settings_ = settings;
        }
        if (st.state == LinkState::Connected) {
            status_.activity = status_.route == Route::Direct ? "Connected directly." : "Connected through the server.";
            status_.reason.clear();
            status_.suggestion.clear();
        } else if (st.state == LinkState::Reconnecting || st.state == LinkState::PeerAway) {
            status_.activity = st.state == LinkState::Reconnecting ? "The connection dropped. Reconnecting..."
                                                                   : "Your friend's connection dropped. Waiting for them...";
        } else if (st.state == LinkState::Waiting) {
            status_.activity = "Waiting for your friend...";
        } else {
            status_.activity.clear();
            status_.reason = st.reason;
        }
    }
}

void OnlineLink::run() {
    if (host_) {
        run_host();
    } else {
        run_guest();
    }
    // Leaving (close()): every way in says goodbye now, so the friend hears at once.
    std::lock_guard lock(mutex_);
    if (room_) {
        room_->close();
    }
    if (direct_host_) {
        direct_host_->close();
    }
    if (direct_join_) {
        direct_join_->close();
    }
    if (status_.state != LinkState::Failed) {
        status_.state = LinkState::Closed;
        status_.activity.clear();
        status_.reason = host_ ? "You stopped hosting." : "You left the race.";
    }
}

void OnlineLink::run_host() {
    const bool server = !options_.server_url.empty() && !options_.lan_only;
    std::mt19937_64 rng(std::random_device{}());
    std::optional<DirectCode> lan_code;  // the same-network code, which the LAN answers give
    {
        std::lock_guard lock(mutex_);
        status_.state = LinkState::Connecting;
        status_.activity = "Getting ready...";
        if (options_.room && server) {
            RoomOptions ro;
            ro.server_url = options_.server_url;
            ro.app_version = options_.app_version;
            ro.game_build = options_.game_build;
            ro.race_settings = options_.race_settings;
            ro.ca_file = options_.ca_file;
            room_ = RoomLink::create_room(ro);
            status_.room.state = RouteStatus::State::Starting;
        } else if (options_.room) {
            status_.room.reason = "No online server is set.";
        }
    }
    const bool listen = options_.direct_code || (options_.room && server && options_.go_direct);
    if (listen) {
        DirectHostOptions dho;
        dho.app_version = options_.app_version;
        dho.game_build = options_.game_build;
        dho.race_settings = options_.race_settings;
        dho.port = options_.port;
        dho.loopback_only = options_.loopback_only;
        std::string error;
        auto dh = DirectHost::start(dho, error);
        std::lock_guard lock(mutex_);
        direct_host_ = std::move(dh);
        if (!direct_host_ && options_.direct_code) {
            status_.direct.state = RouteStatus::State::Failed;
            status_.direct.reason = error;
            status_.direct.suggestion = server ? "Use a room code." : "Ask your friend to host instead.";
        } else if (direct_host_ && options_.direct_code) {
            // A code for a friend on the same network: this computer's address there, no router involved
            // (on the network toward the internet, not a VPN's or a virtual machine's, if it can tell).
            const auto private_ip = [](std::uint32_t ip) {  // 10/8, 172.16/12, 192.168/16
                return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
            };
            std::uint32_t lan_ip = outward_ipv4();
            if (!private_ip(lan_ip)) {
                lan_ip = 0;
                for (const SocketAddress& a : local_addresses()) {
                    if (!a.v6 && private_ip(a.ipv4())) {
                        lan_ip = a.ipv4();
                        break;
                    }
                }
            }
            if (lan_ip) {
                lan_code = DirectCode::make(lan_ip, direct_host_->port(), rng);
                direct_host_->allow("lan", lan_code->key());
                status_.lan.state = RouteStatus::State::Ready;
                status_.lan.code = lan_code->encode();
                status_.lan.detail = format_ipv4(lan_ip) + ":" + std::to_string(lan_code->port);
            } else {
                status_.lan.state = RouteStatus::State::Failed;
                status_.lan.reason = "This computer isn't on a local network: connect it to the router (Wi-Fi or "
                                     "cable).";
            }
            if (options_.lan_only) {
                // The same-network code only.
            } else if (!options_.code_address.empty()) {
                status_.direct.state = RouteStatus::State::Starting;
                // A code for a given address (on a local network, or for tests): no router involved.
                const auto ip = parse_ipv4(options_.code_address);
                const DirectCode code = DirectCode::make(ip.value_or(0x7F000001), direct_host_->port(), rng);
                direct_host_->allow("code", code.key());
                status_.direct.state = RouteStatus::State::Ready;
                status_.direct.code = code.encode();
                status_.direct.detail = format_ipv4(code.ipv4) + ":" + std::to_string(code.port);
            } else {
                status_.direct.state = RouteStatus::State::Starting;
                mapper_ = std::make_unique<PortMapper>(direct_host_->port(), options_.manual_port_forward,
                                                       options_.use_stun);
            }
        }
    }

    // Answers the games on this network that look for a race (net/lan.h), with this one, until a friend
    // is in. (Not being able to is no failure: the same-network code still works.)
    LanHost lan;
    bool lan_answering = false;               // set_race done
    std::optional<RaceSettings> lan_settings;  // with these settings
    const std::string name = options_.lan_name.empty() ? computer_name() : options_.lan_name;
    if (lan_code && options_.lan_port != 0) {
        std::string error;
        if (!lan.start(error, options_.lan_port)) {
            std::lock_guard lock(mutex_);
            status_.lan.log = "Not answering on the network: " + error;
        }
    }

    const std::string room_key = random_key();
    bool code_made = !mapper_;
    bool offered = false, guest_chose_direct = false, guest_chose_relay = false;
    std::int64_t room_friend_at = 0, offered_at = 0;

    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_; })) {
                break;
            }
        }
        const std::int64_t now = now_ms();
        const Route route = [&] {
            std::lock_guard lock(mutex_);
            return status_.route;
        }();

        // The room.
        LinkStatus room;
        if (room_) {
            room = room_->status();
            std::lock_guard lock(mutex_);
            auto& rs = status_.room;
            if (room.state == LinkState::Waiting || room.state == LinkState::Connected || room.state == LinkState::PeerAway) {
                rs.state = RouteStatus::State::Ready;
                rs.code = room.code;
            } else if (finished(room.state)) {
                rs.state = RouteStatus::State::Failed;
                rs.reason = room.reason;
                rs.suggestion = options_.direct_code ? "Give your friend the direct code instead." : "";
            }
        }

        // The LAN answers, with the race as it is now (the host may change the course while waiting).
        if (lan.running() && route != Route::None) {
            lan = LanHost();  // a friend is in: no more answers
        } else if (lan.running()) {
            const std::optional<RaceSettings> settings = [&] {
                std::lock_guard lock(mutex_);
                return settings_;
            }();
            if (!lan_answering || settings != lan_settings) {
                lan_answering = true;
                lan_settings = settings;
                LanRace race;
                race.host.port = lan_code->port;
                race.secret = lan_code->secret;
                race.name = name;
                race.app_version = options_.app_version;
                race.game_build = options_.game_build;
                race.settings = settings.value_or(RaceSettings{});
                lan.set_race(race);
            }
            lan.poll();
        }

        // The router.
        if (!code_made && !mapper_) {
            code_made = true;  // closed: the friend came through the room
        }
        if (!code_made) {
            if (const auto r = mapper_->result()) {
                code_made = true;
                std::lock_guard lock(mutex_);
                auto& ds = status_.direct;
                if (r->reachability.ok() && direct_host_) {
                    const DirectCode code = DirectCode::make(r->reachability.public_ip, r->reachability.public_port, rng);
                    direct_host_->allow("code", code.key());
                    ds.state = RouteStatus::State::Ready;
                    ds.code = code.encode();
                    const bool by_hand = r->mapping.method == RouterMapping::Method::None;
                    ds.detail = std::string(by_hand ? "forwarded by hand" : to_string(r->mapping.method)) + ", " +
                                format_ipv4(code.ipv4) + ":" + std::to_string(code.port);
                } else {
                    ds.state = RouteStatus::State::Failed;
                    ds.reason = r->reachability.reason;
                    ds.suggestion = r->reachability.suggestion;
                }
                ds.log = r->mapping.log;
            }
        }

        if (route == Route::None) {
            // A friend by the direct code: straight in.
            if (direct_host_ && direct_host_->status().state == LinkState::Connected &&
                (direct_host_->joined_via() == "code" || direct_host_->joined_via() == "lan")) {
                choose(Route::Direct, direct_host_.get());
                continue;
            }
            // A friend in the room: offer to go direct, and wait for the guest's decision.
            if (room_ && room.state == LinkState::Connected) {
                if (room_friend_at == 0) {
                    room_friend_at = now;
                    std::lock_guard lock(mutex_);
                    status_.state = LinkState::Connecting;
                    status_.activity = "Your friend is here. Trying a direct connection...";
                }
                if (!offered) {
                    if (!options_.go_direct || !direct_host_) {
                        room_->send_side("relay");  // so the guest doesn't wait for an offer
                        choose(Route::Server, room_.get());
                        continue;
                    }
                    if (code_made || now - room_friend_at >= kMappingWaitMs) {
                        DirectOffer offer;
                        offer.key = room_key;
                        for (auto a : local_addresses()) {
                            if (!a.v6 || direct_host_->ipv6()) {
                                a.port = direct_host_->port();
                                offer.endpoints.push_back(a);
                            }
                        }
                        if (const auto r = mapper_ ? mapper_->result() : std::nullopt; r && r->reachability.ok()) {
                            offer.endpoints.push_back(
                                SocketAddress::ipv4(r->reachability.public_ip, r->reachability.public_port));
                        }
                        if (options_.loopback_only) {
                            offer.endpoints = {SocketAddress::ipv4(0x7F000001, direct_host_->port())};
                        }
                        if (!options_.offer_endpoints.empty()) {
                            offer.endpoints.clear();
                            for (const auto& text : options_.offer_endpoints) {
                                if (const auto a = SocketAddress::parse(text, direct_host_->port())) {
                                    offer.endpoints.push_back(*a);
                                }
                            }
                        }
                        direct_host_->allow("room", room_key);
                        offered = room_->send_side(offer.encode());
                        offered_at = now;
                    }
                }
                for (const auto& side : room_->take_side()) {
                    guest_chose_direct = guest_chose_direct || side == "direct";
                    guest_chose_relay = guest_chose_relay || side == "relay";
                }
                const bool direct_in = direct_host_ && direct_host_->status().state == LinkState::Connected &&
                                       direct_host_->joined_via() == "room";
                if (guest_chose_relay) {
                    choose(Route::Server, room_.get());
                    continue;
                }
                if (guest_chose_direct && direct_in) {
                    choose(Route::Direct, direct_host_.get());
                    continue;
                }
                if (offered && now - offered_at >= options_.direct_wait_ms + kDecisionWaitMs) {
                    // The guest's word got lost: it went direct if its connection is here.
                    choose(direct_in ? Route::Direct : Route::Server,
                           direct_in ? static_cast<host::SerialLink*>(direct_host_.get()) : room_.get());
                    continue;
                }
            } else if (room_friend_at != 0) {
                // The friend left the room before deciding: start over for the next one.
                room_friend_at = 0;
                offered = guest_chose_direct = guest_chose_relay = false;
                if (direct_host_) {
                    direct_host_->disallow("room");
                }
            }

            std::lock_guard lock(mutex_);
            const bool room_up = status_.room.state == RouteStatus::State::Ready ||
                                 status_.room.state == RouteStatus::State::Starting;
            const bool direct_up = status_.direct.state == RouteStatus::State::Ready ||
                                   status_.direct.state == RouteStatus::State::Starting;
            const bool lan_up = status_.lan.state == RouteStatus::State::Ready;  // a friend nearby can come
            if (!room_up && !direct_up && !lan_up) {
                status_.state = LinkState::Failed;
                status_.activity.clear();
                const bool room_failed = status_.room.state == RouteStatus::State::Failed;
                const bool direct_failed = status_.direct.state == RouteStatus::State::Failed;
                if (room_failed && direct_failed) {
                    status_.reason = "Neither a room code nor a direct code could be made. " + status_.room.reason +
                                     " " + status_.direct.reason;
                    status_.suggestion = status_.direct.suggestion;
                } else if (room_failed) {
                    status_.reason = status_.room.reason;
                    status_.suggestion = status_.room.suggestion;
                } else if (!status_.direct.reason.empty()) {
                    status_.reason = status_.direct.reason;
                    status_.suggestion = status_.direct.suggestion;
                } else if (options_.lan_only && !status_.lan.reason.empty()) {
                    status_.reason = status_.lan.reason;
                    status_.suggestion.clear();
                } else {
                    status_.reason = status_.room.reason;
                    status_.suggestion = status_.room.suggestion;
                }
            } else if (room_friend_at == 0) {
                const bool ready = status_.room.state == RouteStatus::State::Ready ||
                                   status_.direct.state == RouteStatus::State::Ready || lan_up;
                status_.state = ready ? LinkState::Waiting : LinkState::Connecting;
                status_.activity = ready ? "Waiting for your friend..." : "Getting ready...";
            }
        } else {
            publish_active();
        }
    }
}

void OnlineLink::run_guest() {
    if (!failure_.empty()) {
        std::lock_guard lock(mutex_);
        status_.state = LinkState::Failed;
        status_.reason = failure_;
        status_.suggestion = "Ask your friend for the code again.";
        return;
    }
    {
        std::lock_guard lock(mutex_);
        status_.state = LinkState::Connecting;
        if (invite_->kind == Invite::Kind::Room) {
            RoomOptions ro;
            ro.server_url = options_.server_url;
            ro.app_version = options_.app_version;
            ro.game_build = options_.game_build;
            ro.ca_file = options_.ca_file;
            room_ = RoomLink::join_room(ro, invite_->code);
            status_.room.state = RouteStatus::State::Starting;
            status_.room.code = invite_->code;
            status_.activity = "Connecting to the server...";
        } else {
            const DirectCode code = *DirectCode::decode(invite_->code);
            DirectJoinOptions dj;
            dj.app_version = options_.app_version;
            dj.game_build = options_.game_build;
            dj.purpose = "code";
            dj.key = code.key();
            dj.endpoints = {SocketAddress::ipv4(code.ipv4, code.port)};
            direct_join_ = DirectJoin::start(dj);
            status_.direct.state = RouteStatus::State::Starting;
            status_.direct.code = invite_->code;
            status_.direct.detail = dj.endpoints.front().to_string();
            status_.activity = "Connecting to your friend's game...";
        }
    }

    std::int64_t room_friend_at = 0, close_room_at = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_; })) {
                break;
            }
        }
        const std::int64_t now = now_ms();
        const Route route = [&] {
            std::lock_guard lock(mutex_);
            return status_.route;
        }();

        if (invite_->kind == Invite::Kind::Direct) {
            const LinkStatus st = direct_join_->status();
            if (route == Route::None && st.state == LinkState::Connected) {
                std::lock_guard lock(mutex_);
                status_.direct.state = RouteStatus::State::Ready;
                status_.route = Route::Direct;
                active_ = direct_join_.get();
            }
            if (route == Route::None && finished(st.state)) {
                std::lock_guard lock(mutex_);
                status_.state = st.state;
                status_.reason = st.reason;
                status_.suggestion = direct_join_->suggestion();
                status_.direct.state = RouteStatus::State::Failed;
                status_.direct.reason = st.reason;
                status_.direct.suggestion = status_.suggestion;
                status_.activity.clear();
                continue;
            }
            if (route != Route::None || st.state == LinkState::Connected) {
                publish_active();
            }
            continue;
        }

        // By a room code.
        if (close_room_at && now >= close_room_at) {
            close_room_at = 0;
            std::unique_ptr<RoomLink> room;
            {
                std::lock_guard lock(mutex_);
                room = std::move(room_);
            }
            cleanups_.emplace_back([r = std::move(room)]() mutable { r.reset(); });
        }
        if (route != Route::None) {
            publish_active();
            continue;
        }
        const LinkStatus room = room_->status();
        {
            std::lock_guard lock(mutex_);
            if (room.state == LinkState::Connected) {
                status_.room.state = RouteStatus::State::Ready;
            } else if (finished(room.state) || room.state == LinkState::PeerLeft) {
                status_.room.state = RouteStatus::State::Failed;
                status_.room.reason = room.reason;
                status_.state = room.state;
                status_.reason = room.reason;
                status_.activity.clear();
                if (room.reason.find("same version") == std::string::npos && room.state == LinkState::Failed) {
                    status_.suggestion = "Check the code with your friend.";
                }
                continue;
            }
        }
        if (room.state != LinkState::Connected) {
            continue;
        }
        if (room_friend_at == 0) {
            room_friend_at = now;
            std::lock_guard lock(mutex_);
            status_.activity = "Your friend is here. Trying a direct connection...";
        }
        if (!direct_join_ && !options_.go_direct) {
            room_->send_side("relay");  // no need to wait for the host's offer
            choose(Route::Server, room_.get());
            continue;
        }
        if (!direct_join_) {
            std::optional<DirectOffer> offer;
            bool host_says_relay = false;
            for (const auto& side : room_->take_side()) {
                host_says_relay = host_says_relay || side == "relay";
                if (!offer) {
                    offer = DirectOffer::decode(side);
                }
            }
            if (host_says_relay) {
                choose(Route::Server, room_.get());
                continue;
            }
            if (offer && options_.go_direct && !offer->endpoints.empty()) {
                DirectJoinOptions dj;
                dj.app_version = options_.app_version;
                dj.game_build = options_.game_build;
                dj.purpose = "room";
                dj.key = offer->key;
                dj.endpoints = offer->endpoints;
                dj.timeout_ms = options_.direct_wait_ms;
                std::lock_guard lock(mutex_);
                direct_join_ = DirectJoin::start(dj);
                status_.direct.state = RouteStatus::State::Starting;
            } else if (offer || now - room_friend_at >= kOfferWaitMs) {
                room_->send_side("relay");
                choose(Route::Server, room_.get());
            }
            continue;
        }
        const LinkStatus dj = direct_join_->status();
        if (dj.state == LinkState::Connected) {
            room_->send_side("direct");
            {
                std::lock_guard lock(mutex_);
                status_.direct.state = RouteStatus::State::Ready;
                if (const auto ep = direct_join_->endpoint()) {
                    status_.direct.detail = ep->to_string();
                }
            }
            choose(Route::Direct, direct_join_.get());
            close_room_at = now + 2000;  // after the host has had our word
        } else if (finished(dj.state) || dj.state == LinkState::PeerLeft) {
            {
                std::lock_guard lock(mutex_);
                status_.direct.state = RouteStatus::State::Failed;
                status_.direct.reason = dj.reason;
            }
            room_->send_side("relay");
            choose(Route::Server, room_.get());
        }
    }
}

}  // namespace vette::net
